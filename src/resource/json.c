#include "json.h"

#include <stdio.h>
#include <stdlib.h>

typedef struct {
  const char *s, *end;
  int line;
  char *err;
  size_t err_cap;
  bool failed;
} parser;

static cwj *mk(cwj_type t) {
  cwj *v = cw_calloc(1, sizeof *v, CW_MEM_RESOURCE);
  v->type = t;
  return v;
}

static void fail(parser *p, const char *what) {
  if (p->failed) return;
  p->failed = true;
  if (p->err && p->err_cap) snprintf(p->err, p->err_cap, "line %d: %s", p->line, what);
}

static void ws(parser *p) {
  while (p->s < p->end && (*p->s == ' ' || *p->s == '\t' || *p->s == '\n' || *p->s == '\r')) {
    if (*p->s == '\n') p->line++;
    p->s++;
  }
}

static void grow(cwj *v) {
  if (v->count < v->cap) return;
  v->cap = v->cap ? v->cap * 2 : 8;
  v->items = cw_realloc(v->items, (size_t)v->cap * sizeof *v->items, CW_MEM_RESOURCE);
  if (v->type == CWJ_OBJ) v->keys = cw_realloc(v->keys, (size_t)v->cap * sizeof *v->keys, CW_MEM_RESOURCE);
}

static void put_utf8(char **o, uint32_t c) {
  char *d = *o;
  if (c < 0x80) *d++ = (char)c;
  else if (c < 0x800) { *d++ = (char)(0xC0 | (c >> 6)); *d++ = (char)(0x80 | (c & 63)); }
  else if (c < 0x10000) { *d++ = (char)(0xE0 | (c >> 12)); *d++ = (char)(0x80 | ((c >> 6) & 63)); *d++ = (char)(0x80 | (c & 63)); }
  else {
    *d++ = (char)(0xF0 | (c >> 18)); *d++ = (char)(0x80 | ((c >> 12) & 63));
    *d++ = (char)(0x80 | ((c >> 6) & 63)); *d++ = (char)(0x80 | (c & 63));
  }
  *o = d;
}

static int hex4(const char *s) {
  int v = 0;
  for (int i = 0; i < 4; i++) {
    char c = s[i];
    v <<= 4;
    if (c >= '0' && c <= '9') v |= c - '0';
    else if (c >= 'a' && c <= 'f') v |= c - 'a' + 10;
    else if (c >= 'A' && c <= 'F') v |= c - 'A' + 10;
    else return -1;
  }
  return v;
}

static char *parse_string(parser *p) {
  /* p->s at the opening quote */
  const char *q = ++p->s;
  size_t n = 0;
  while (q < p->end && *q != '"') { if (*q == '\\') q++; q++; n++; }
  if (q >= p->end) { fail(p, "unterminated string"); return NULL; }
  char *out = cw_alloc(n * 4 + 1, CW_MEM_RESOURCE), *o = out;
  while (*p->s != '"') {
    char c = *p->s++;
    if (c != '\\') { *o++ = c; continue; }
    char e = *p->s++;
    switch (e) {
    case 'n': *o++ = '\n'; break;
    case 't': *o++ = '\t'; break;
    case 'r': *o++ = '\r'; break;
    case 'b': *o++ = '\b'; break;
    case 'f': *o++ = '\f'; break;
    case 'u': {
      if (p->end - p->s < 4) { fail(p, "bad \\u escape"); cw_free(out); return NULL; }
      int c1 = hex4(p->s);
      p->s += 4;
      if (c1 < 0) { fail(p, "bad \\u escape"); cw_free(out); return NULL; }
      uint32_t cp = (uint32_t)c1;
      if (cp >= 0xD800 && cp < 0xDC00 && p->end - p->s >= 6 && p->s[0] == '\\' && p->s[1] == 'u') {
        int c2 = hex4(p->s + 2);
        if (c2 >= 0xDC00 && c2 < 0xE000) { cp = 0x10000 + ((cp - 0xD800) << 10) + ((uint32_t)c2 - 0xDC00); p->s += 6; }
      }
      put_utf8(&o, cp);
      break;
    }
    default: *o++ = e; break; /* \" \\ \/ */
    }
  }
  p->s++;
  *o = 0;
  return out;
}

static cwj *parse_value(parser *p, int depth) {
  ws(p);
  if (p->s >= p->end) { fail(p, "unexpected end"); return NULL; }
  if (depth > 256) { fail(p, "nested too deeply"); return NULL; }
  char c = *p->s;
  if (c == '{' || c == '[') {
    bool obj = c == '{';
    cwj *v = mk(obj ? CWJ_OBJ : CWJ_ARR);
    p->s++;
    ws(p);
    if (p->s < p->end && *p->s == (obj ? '}' : ']')) { p->s++; return v; }
    for (;;) {
      char *key = NULL;
      if (obj) {
        ws(p);
        if (p->s >= p->end || *p->s != '"') { fail(p, "expected a key"); cwj_free(v); return NULL; }
        key = parse_string(p);
        if (!key) { cwj_free(v); return NULL; }
        ws(p);
        if (p->s >= p->end || *p->s != ':') { fail(p, "expected ':'"); cw_free(key); cwj_free(v); return NULL; }
        p->s++;
      }
      cwj *item = parse_value(p, depth + 1);
      if (!item) { cw_free(key); cwj_free(v); return NULL; }
      grow(v);
      if (obj) v->keys[v->count] = key;
      v->items[v->count++] = item;
      ws(p);
      if (p->s < p->end && *p->s == ',') { p->s++; continue; }
      if (p->s < p->end && *p->s == (obj ? '}' : ']')) { p->s++; return v; }
      fail(p, obj ? "expected ',' or '}'" : "expected ',' or ']'");
      cwj_free(v);
      return NULL;
    }
  }
  if (c == '"') {
    char *s = parse_string(p);
    if (!s) return NULL;
    cwj *v = mk(CWJ_STR);
    v->text = s;
    return v;
  }
  if (c == '-' || (c >= '0' && c <= '9')) {
    const char *b = p->s;
    while (p->s < p->end && (strchr("+-.eE", *p->s) || (*p->s >= '0' && *p->s <= '9'))) p->s++;
    size_t n = (size_t)(p->s - b);
    cwj *v = mk(CWJ_NUM);
    v->text = cw_alloc(n + 1, CW_MEM_RESOURCE);
    memcpy(v->text, b, n);
    v->text[n] = 0;
    char *e;
    v->num = strtod(v->text, &e);
    if (*e) { fail(p, "bad number"); cwj_free(v); return NULL; }
    return v;
  }
  if (p->end - p->s >= 4 && !memcmp(p->s, "true", 4)) { p->s += 4; return cwj_bool(true); }
  if (p->end - p->s >= 5 && !memcmp(p->s, "false", 5)) { p->s += 5; return cwj_bool(false); }
  if (p->end - p->s >= 4 && !memcmp(p->s, "null", 4)) { p->s += 4; return mk(CWJ_NULL); }
  fail(p, "unexpected character");
  return NULL;
}

cwj *cwj_parse(const char *s, size_t len, char *err, size_t err_cap) {
  parser p = { s, s + len, 1, err, err_cap, false };
  if (len >= 3 && (uint8_t)s[0] == 0xEF && (uint8_t)s[1] == 0xBB && (uint8_t)s[2] == 0xBF) p.s += 3; /* BOM */
  cwj *v = parse_value(&p, 0);
  if (!v) return NULL;
  ws(&p);
  while (p.s < p.end && *p.s == 0) p.s++; /* GLB JSON chunks may be NUL-padded */
  ws(&p);
  if (p.s != p.end) { fail(&p, "trailing characters"); cwj_free(v); return NULL; }
  return v;
}

void cwj_free(cwj *v) {
  if (!v) return;
  for (int i = 0; i < v->count; i++) {
    if (v->keys) cw_free(v->keys[i]);
    cwj_free(v->items[i]);
  }
  cw_free(v->keys);
  cw_free(v->items);
  cw_free(v->text);
  cw_free(v);
}

/* ── writing ─────────────────────────────────────────────────────── */
typedef struct { char *d; size_t n, cap; } sbuf;
static void sb_put(sbuf *b, const char *s, size_t n) {
  if (b->n + n + 1 > b->cap) {
    while (b->n + n + 1 > b->cap) b->cap = b->cap ? b->cap * 2 : 1024;
    b->d = cw_realloc(b->d, b->cap, CW_MEM_RESOURCE);
  }
  memcpy(b->d + b->n, s, n);
  b->n += n;
  b->d[b->n] = 0;
}
static void sb_str(sbuf *b, const char *s) {
  sb_put(b, "\"", 1);
  for (const char *c = s; *c; c++) {
    unsigned char u = (unsigned char)*c;
    if (u == '"') sb_put(b, "\\\"", 2);
    else if (u == '\\') sb_put(b, "\\\\", 2);
    else if (u == '\n') sb_put(b, "\\n", 2);
    else if (u == '\r') sb_put(b, "\\r", 2);
    else if (u == '\t') sb_put(b, "\\t", 2);
    else if (u < 0x20) { char t[8]; snprintf(t, sizeof t, "\\u%04x", u); sb_put(b, t, 6); }
    else sb_put(b, c, 1);
  }
  sb_put(b, "\"", 1);
}
static void write_value(sbuf *b, const cwj *v) {
  char t[64];
  switch (v->type) {
  case CWJ_NULL: sb_put(b, "null", 4); break;
  case CWJ_BOOL: v->b ? sb_put(b, "true", 4) : sb_put(b, "false", 5); break;
  case CWJ_NUM:
    if (v->text) sb_put(b, v->text, strlen(v->text));
    else {
      double d = v->num;
      if (d == (double)(int64_t)d && d > -9.0e15 && d < 9.0e15) snprintf(t, sizeof t, "%lld", (long long)d);
      else snprintf(t, sizeof t, "%.17g", d);
      sb_put(b, t, strlen(t));
    }
    break;
  case CWJ_STR: sb_str(b, v->text); break;
  case CWJ_ARR:
  case CWJ_OBJ:
    sb_put(b, v->type == CWJ_OBJ ? "{" : "[", 1);
    for (int i = 0; i < v->count; i++) {
      if (i) sb_put(b, ",", 1);
      if (v->type == CWJ_OBJ) { sb_str(b, v->keys[i]); sb_put(b, ":", 1); }
      write_value(b, v->items[i]);
    }
    sb_put(b, v->type == CWJ_OBJ ? "}" : "]", 1);
    break;
  }
}
char *cwj_write(const cwj *v, size_t *len) {
  sbuf b = { 0 };
  sb_put(&b, "", 0);
  write_value(&b, v);
  if (len) *len = b.n;
  return b.d;
}

/* ── building and editing ────────────────────────────────────────── */
cwj *cwj_null(void) { return mk(CWJ_NULL); }
cwj *cwj_bool(bool b) { cwj *v = mk(CWJ_BOOL); v->b = b; return v; }
cwj *cwj_num(double n) { cwj *v = mk(CWJ_NUM); v->num = n; return v; }
cwj *cwj_numf(float f) {
  cwj *v = mk(CWJ_NUM);
  v->num = f;
  char t[32];
  if (f == (float)(int32_t)f && f > -1.0e7f && f < 1.0e7f) snprintf(t, sizeof t, "%d", (int)f);
  else snprintf(t, sizeof t, "%.9g", (double)f);
  v->text = cw_strdup(t, CW_MEM_RESOURCE);
  return v;
}
cwj *cwj_str(const char *s) { cwj *v = mk(CWJ_STR); v->text = cw_strdup(s, CW_MEM_RESOURCE); return v; }
cwj *cwj_arr(void) { return mk(CWJ_ARR); }
cwj *cwj_obj(void) { return mk(CWJ_OBJ); }

cwj *cwj_clone(const cwj *v) {
  if (!v) return NULL;
  cwj *c = mk(v->type);
  c->b = v->b;
  c->num = v->num;
  if (v->text) c->text = cw_strdup(v->text, CW_MEM_RESOURCE);
  for (int i = 0; i < v->count; i++) {
    grow(c);
    if (v->type == CWJ_OBJ) c->keys[i] = cw_strdup(v->keys[i], CW_MEM_RESOURCE);
    c->items[i] = cwj_clone(v->items[i]);
    c->count++;
  }
  return c;
}

cwj *cwj_get(const cwj *o, const char *key) {
  if (!o || o->type != CWJ_OBJ) return NULL;
  for (int i = 0; i < o->count; i++)
    if (!strcmp(o->keys[i], key)) return o->items[i];
  return NULL;
}
cwj *cwj_at(const cwj *a, int i) {
  if (!a || a->type != CWJ_ARR || i < 0 || i >= a->count) return NULL;
  return a->items[i];
}
void cwj_set(cwj *o, const char *key, cwj *val) {
  CW_ASSERT(o && o->type == CWJ_OBJ, "cwj_set needs an object");
  for (int i = 0; i < o->count; i++)
    if (!strcmp(o->keys[i], key)) { cwj_free(o->items[i]); o->items[i] = val; return; }
  grow(o);
  o->keys[o->count] = cw_strdup(key, CW_MEM_RESOURCE);
  o->items[o->count++] = val;
}
void cwj_remove(cwj *o, const char *key) {
  if (!o || o->type != CWJ_OBJ) return;
  for (int i = 0; i < o->count; i++)
    if (!strcmp(o->keys[i], key)) {
      cw_free(o->keys[i]);
      cwj_free(o->items[i]);
      memmove(o->keys + i, o->keys + i + 1, (size_t)(o->count - i - 1) * sizeof *o->keys);
      memmove(o->items + i, o->items + i + 1, (size_t)(o->count - i - 1) * sizeof *o->items);
      o->count--;
      return;
    }
}
void cwj_push(cwj *a, cwj *val) {
  CW_ASSERT(a && a->type == CWJ_ARR, "cwj_push needs an array");
  grow(a);
  a->items[a->count++] = val;
}

double cwj_num_or(const cwj *v, double def) { return v && v->type == CWJ_NUM ? v->num : def; }
int64_t cwj_int(const cwj *o, const char *key, int64_t def) {
  cwj *v = cwj_get(o, key);
  return v && v->type == CWJ_NUM ? (int64_t)v->num : def;
}
const char *cwj_string(const cwj *o, const char *key) {
  cwj *v = cwj_get(o, key);
  return v && v->type == CWJ_STR ? v->text : NULL;
}

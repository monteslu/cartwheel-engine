/* A small JSON reader for the game's debug-channel replies (tree, changes,
 * props, get). Values are a malloc'd DOM; pj_free releases it. */
#include <stdlib.h>
#include <string.h>

#include "play.h"

typedef struct { const char *s; } rd;

static void ws(rd *r) {
  while (*r->s == ' ' || *r->s == '\t' || *r->s == '\n' || *r->s == '\r') r->s++;
}

static pj *alloc(pj_type t) {
  pj *v = calloc(1, sizeof *v);
  v->t = t;
  return v;
}

static void put_utf8(char **w, unsigned cp) {
  char *o = *w;
  if (cp < 0x80) *o++ = (char)cp;
  else if (cp < 0x800) *o++ = (char)(0xc0 | cp >> 6), *o++ = (char)(0x80 | (cp & 63));
  else *o++ = (char)(0xe0 | cp >> 12), *o++ = (char)(0x80 | ((cp >> 6) & 63)), *o++ = (char)(0x80 | (cp & 63));
  *w = o;
}

static char *string(rd *r) {
  if (*r->s != '"') return NULL;
  const char *p = ++r->s;
  size_t n = 0;
  while (p[n] && p[n] != '"') n += p[n] == '\\' && p[n + 1] ? 2 : 1;
  char *out = malloc(n + 1), *w = out;
  while (*r->s && *r->s != '"') {
    char c = *r->s++;
    if (c != '\\') {
      *w++ = c;
      continue;
    }
    c = *r->s++;
    switch (c) {
    case 'n': *w++ = '\n'; break;
    case 't': *w++ = '\t'; break;
    case 'r': *w++ = '\r'; break;
    case 'b': *w++ = '\b'; break;
    case 'f': *w++ = '\f'; break;
    case 'u': {
      unsigned cp = (unsigned)strtoul((char[5]){ r->s[0], r->s[1], r->s[2], r->s[3], 0 }, NULL, 16);
      r->s += 4;
      put_utf8(&w, cp);
      break;
    }
    case 0: r->s--; break;
    default: *w++ = c;
    }
  }
  *w = 0;
  if (*r->s == '"') r->s++;
  return out;
}

static pj *value(rd *r, int depth) {
  ws(r);
  if (depth > 200) return NULL;
  char c = *r->s;
  if (c == '{' || c == '[') {
    bool obj = c == '{';
    pj *v = alloc(obj ? PJ_OBJ : PJ_ARR);
    r->s++;
    int cap = 0;
    for (;;) {
      ws(r);
      if (*r->s == (obj ? '}' : ']')) {
        r->s++;
        return v;
      }
      char *key = NULL;
      if (obj) {
        key = string(r);
        ws(r);
        if (!key || *r->s != ':') {
          free(key);
          pj_free(v);
          return NULL;
        }
        r->s++;
      }
      pj *item = value(r, depth + 1);
      if (!item) {
        free(key);
        pj_free(v);
        return NULL;
      }
      if (v->count == cap) {
        cap = cap ? cap * 2 : 8;
        v->items = realloc(v->items, sizeof *v->items * (size_t)cap);
        if (obj) v->keys = realloc(v->keys, sizeof *v->keys * (size_t)cap);
      }
      v->items[v->count] = item;
      if (obj) v->keys[v->count] = key;
      v->count++;
      ws(r);
      if (*r->s == ',') r->s++;
    }
  }
  if (c == '"') {
    pj *v = alloc(PJ_STR);
    v->s = string(r);
    return v;
  }
  if (!strncmp(r->s, "true", 4) || !strncmp(r->s, "false", 5)) {
    pj *v = alloc(PJ_BOOL);
    v->b = c == 't';
    r->s += v->b ? 4 : 5;
    return v;
  }
  if (!strncmp(r->s, "null", 4)) {
    r->s += 4;
    return alloc(PJ_NULL);
  }
  char *end;
  double d = strtod(r->s, &end);
  if (end == r->s) return NULL;
  r->s = end;
  pj *v = alloc(PJ_NUM);
  v->n = d;
  return v;
}

pj *pj_parse(const char *text) {
  if (!text) return NULL;
  rd r = { text };
  return value(&r, 0);
}

void pj_free(pj *v) {
  if (!v) return;
  for (int i = 0; i < v->count; i++) {
    pj_free(v->items[i]);
    if (v->keys) free(v->keys[i]);
  }
  free(v->items);
  free(v->keys);
  free(v->s);
  free(v);
}

pj *pj_get(const pj *o, const char *key) {
  if (!o || o->t != PJ_OBJ) return NULL;
  for (int i = 0; i < o->count; i++)
    if (!strcmp(o->keys[i], key)) return o->items[i];
  return NULL;
}

pj *pj_at(const pj *a, int i) { return a && (a->t == PJ_ARR || a->t == PJ_OBJ) && i >= 0 && i < a->count ? a->items[i] : NULL; }

const char *pj_str(const pj *o, const char *key) {
  pj *v = pj_get(o, key);
  return v && v->t == PJ_STR ? v->s : NULL;
}

double pj_num(const pj *o, const char *key, double def) {
  pj *v = pj_get(o, key);
  return v && v->t == PJ_NUM ? v->n : def;
}

bool pj_bool(const pj *o, const char *key, bool def) {
  pj *v = pj_get(o, key);
  return v && v->t == PJ_BOOL ? v->b : def;
}

/* Values and properties: every node property reachable by name. */
#include <stdio.h>
#include <stdlib.h>

#include "scene_internal.h"

const char *cw_type_name(cw_type t) {
  static const char *n[] = { "none", "bool", "int", "float", "vec2", "vec3", "vec4", "quat", "color",
                             "string", "node", "res", "enum" };
  return t < CW_T_COUNT ? n[t] : "?";
}

static int fmt_float(char *b, int cap, double f) {
  /* shortest round-trippable form for floats; integers print without decimals */
  char tmp[40];
  for (int prec = 6; prec <= 17; prec++) {
    snprintf(tmp, sizeof tmp, "%.*g", prec, f);
    if (strtod(tmp, NULL) == f || (float)strtod(tmp, NULL) == (float)f) break;
  }
  return snprintf(b, (size_t)cap, "%s", tmp);
}

int cw_value_format(const cw_value *v, char *b, int cap) {
  int n = 0;
#define P(...) (n += snprintf(b + n, (size_t)(cap > n ? cap - n : 0), __VA_ARGS__))
#define F(x) (n += fmt_float(b + n, cap > n ? cap - n : 0, (x)))
  switch (v->type) {
  case CW_T_BOOL: P("%s", v->b ? "true" : "false"); break;
  case CW_T_INT: P("%lld", (long long)v->i); break;
  case CW_T_FLOAT: F(v->f); break;
  case CW_T_VEC2: case CW_T_VEC3: case CW_T_VEC4: case CW_T_QUAT: case CW_T_COLOR: {
    static const char *nm[] = { [CW_T_VEC2] = "vec2", [CW_T_VEC3] = "vec3", [CW_T_VEC4] = "vec4", [CW_T_QUAT] = "quat", [CW_T_COLOR] = "color" };
    int k = v->type == CW_T_VEC2 ? 2 : v->type == CW_T_VEC3 ? 3 : 4;
    P("%s(", nm[v->type]);
    for (int i = 0; i < k; i++) { if (i) P(", "); F(v->v[i]); }
    P(")");
    break;
  }
  case CW_T_STRING: {
    P("\"");
    for (const char *s = v->s ? v->s : ""; *s; s++) {
      if (*s == '"' || *s == '\\') P("\\%c", *s);
      else if (*s == '\n') P("\\n");
      else P("%c", *s);
    }
    P("\"");
    break;
  }
  case CW_T_NODE: {
    char path[512];
    if (cw_node_path(v->node, (cw_node){ 0 }, path, sizeof path) < 0) path[0] = 0;
    P("node(\"%s\")", path);
    break;
  }
  case CW_T_RES: P("res(0x%llx)", (unsigned long long)v->res); break;
  case CW_T_ENUM: P("%lld", (long long)v->i); break;
  default: P("null"); break;
  }
#undef P
#undef F
  return n;
}

static const char *skip_ws(const char *s) {
  while (*s == ' ' || *s == '\t') s++;
  return s;
}

bool cw_value_parse(const char *text, cw_type expect, cw_value *out) {
  const char *s = skip_ws(text);
  memset(out, 0, sizeof *out);
  if (!strncmp(s, "true", 4) || !strncmp(s, "false", 5)) {
    out->type = CW_T_BOOL;
    out->b = s[0] == 't';
  } else if (*s == '"') {
    char buf[4096];
    int k = 0;
    for (s++; *s && *s != '"' && k < (int)sizeof buf - 1; s++) {
      if (*s == '\\' && s[1]) { s++; buf[k++] = *s == 'n' ? '\n' : *s; }
      else buf[k++] = *s;
    }
    buf[k] = 0;
    out->type = CW_T_STRING;
    out->s = cw_intern(buf);
  } else if (!strncmp(s, "res(", 4)) {
    out->type = CW_T_RES;
    out->res = strtoull(s + 4, NULL, 0);
  } else if (!strncmp(s, "node(", 5)) {
    out->type = CW_T_NODE; /* resolved now, in the current tree (the scene loader resolves its own, deferred) */
    const char *q = strchr(s, '"');
    if (q) {
      char buf[512];
      int k = 0;
      for (q++; *q && *q != '"' && k < 511; q++) buf[k++] = *q;
      buf[k] = 0;
      out->node = cw_node_get(cw_tree_root(cw_current_tree()), buf);
    }
  } else if (!strncmp(s, "vec2(", 5) || !strncmp(s, "vec3(", 5) || !strncmp(s, "vec4(", 5) || !strncmp(s, "quat(", 5) ||
             !strncmp(s, "color(", 6)) {
    out->type = s[0] == 'c' ? CW_T_COLOR : s[0] == 'q' ? CW_T_QUAT : s[3] == '2' ? CW_T_VEC2 : s[3] == '3' ? CW_T_VEC3 : CW_T_VEC4;
    const char *p = strchr(s, '(') + 1;
    int got = 0;
    for (int i = 0; i < 4; i++, got++) {
      char *end;
      double d = strtod(p, &end);
      if (end == p) break;
      out->v[i] = (float)d;
      p = skip_ws(end);
      if (*p == ',') p++;
    }
    if (out->type == CW_T_COLOR && got < 4) out->v[3] = 1; /* color(r, g, b) is opaque */
  } else {
    char *end;
    double d = strtod(s, &end);
    if (end == s) return false;
    bool is_int = !memchr(s, '.', (size_t)(end - s)) && !memchr(s, 'e', (size_t)(end - s));
    if (is_int && expect != CW_T_FLOAT) { out->type = CW_T_INT; out->i = strtoll(s, NULL, 0); }
    else { out->type = CW_T_FLOAT; out->f = d; }
  }
  return true;
}

/* convert where it is lossless or obvious (int <-> float, vec3 <-> color...) */
static bool coerce(cw_value *v, cw_type want) {
  if (v->type == want || want == CW_T_NONE) return true;
  if (want == CW_T_FLOAT && v->type == CW_T_INT) { v->f = (double)v->i; v->type = want; return true; }
  if ((want == CW_T_INT || want == CW_T_ENUM) && (v->type == CW_T_INT || v->type == CW_T_ENUM)) { v->type = want; return true; }
  if (want == CW_T_INT && v->type == CW_T_FLOAT && v->f == (double)(int64_t)v->f) { v->i = (int64_t)v->f; v->type = want; return true; }
  if (want == CW_T_BOOL && v->type == CW_T_INT) { v->b = v->i != 0; v->type = want; return true; }
  if (want == CW_T_COLOR && (v->type == CW_T_VEC3 || v->type == CW_T_VEC4)) { if (v->type == CW_T_VEC3) v->v[3] = 1; v->type = want; return true; }
  if ((want == CW_T_VEC3 || want == CW_T_VEC4) && v->type == CW_T_COLOR) { v->type = want; return true; }
  if (want == CW_T_QUAT && v->type == CW_T_VEC4) { v->type = want; return true; }
  return false;
}

const cw_prop *cw_node_find_prop(cw_node n, const char *name, const cw_node_type **owner) {
  const cw_node_type *t = cw_node_get_type(n);
  for (; t; t = t->base)
    for (const cw_prop *p = t->props; p && p->name; p++)
      if (!strcmp(p->name, name)) {
        if (owner) *owner = t;
        return p;
      }
  return NULL;
}

int cw_node_props(cw_node n, const cw_prop **out, const cw_node_type **owners, int cap) {
  const cw_node_type *chain[CW__MAX_TYPE_DEPTH];
  const cw_node_type *t = cw_node_get_type(n);
  if (!t) return 0;
  for (const cw_node_type *c = t; c; c = c->base) chain[c->depth] = c;
  int k = 0;
  for (int d = 0; d <= t->depth; d++)
    for (const cw_prop *p = chain[d]->props; p && p->name; p++) {
      if (k < cap) { out[k] = p; if (owners) owners[k] = chain[d]; }
      k++;
    }
  return k;
}

static void field_get(const cw_prop *p, const uint8_t *f, cw_value *o) {
  o->type = p->type;
  switch (p->type) {
  case CW_T_BOOL: o->b = *(const bool *)f; break;
  case CW_T_INT: case CW_T_ENUM:
    o->i = p->size == 8 ? *(const int64_t *)f : p->size == 2 ? *(const int16_t *)f : p->size == 1 ? *(const int8_t *)f : *(const int32_t *)f;
    break;
  case CW_T_FLOAT: o->f = p->size == 8 ? *(const double *)f : *(const float *)f; break;
  case CW_T_VEC2: case CW_T_VEC3: case CW_T_VEC4: case CW_T_QUAT: case CW_T_COLOR:
    memcpy(o->v, f, (size_t)CW_MIN(p->size, 16));
    if (p->type == CW_T_COLOR && p->size == 12) o->v[3] = 1;
    break;
  case CW_T_STRING: { /* a cw_value's string is pinned (values are copied anywhere) */
    o->s = cw_intern_pin(*(const char *const *)f);
    break;
  }
  case CW_T_NODE: o->node = *(const cw_node *)f; break;
  case CW_T_RES: o->res = *(const uint64_t *)f; break;
  default: break;
  }
}

static void field_set(const cw_prop *p, uint8_t *f, const cw_value *v) {
  switch (p->type) {
  case CW_T_BOOL: *(bool *)f = v->b; break;
  case CW_T_INT: case CW_T_ENUM:
    if (p->size == 8) *(int64_t *)f = v->i;
    else if (p->size == 2) *(int16_t *)f = (int16_t)v->i;
    else if (p->size == 1) *(int8_t *)f = (int8_t)v->i;
    else *(int32_t *)f = (int32_t)v->i;
    break;
  case CW_T_FLOAT:
    if (p->size == 8) *(double *)f = v->f;
    else *(float *)f = (float)v->f;
    break;
  case CW_T_VEC2: case CW_T_VEC3: case CW_T_VEC4: case CW_T_QUAT: case CW_T_COLOR: memcpy(f, v->v, (size_t)CW_MIN(p->size, 16)); break;
  case CW_T_STRING: { /* the old string may be a counted one a setter stored */
    const char *old = *(const char **)f;
    *(const char **)f = v->s ? cw_intern(v->s) : NULL;
    cw_intern_unref(old);
    break;
  }
  case CW_T_NODE: *(cw_node *)f = v->node; break;
  case CW_T_RES: *(uint64_t *)f = v->res; break;
  default: break;
  }
}

bool cw_node_get_prop(cw_node n, const char *name, cw_value *out) {
  const cw_node_type *owner;
  const cw_prop *p = cw_node_find_prop(n, name, &owner);
  if (!p) return false;
  void *data = cw_node_data_t(n, owner);
  if (p->get) p->get(n, data, out);
  else field_get(p, (const uint8_t *)data + p->offset, out);
  return true;
}

/* A float property set outside its range is clamped to it. Say so, once per
 * node and property (a small table of the last 128: past that an old entry
 * is forgotten and may warn again). */
static void warn_clamped(cw_node n, const char *name, const cw_prop *p, float asked) {
  static struct { uint64_t node; const cw_prop *prop; } seen[128];
  static unsigned next;
  for (unsigned i = 0; i < 128; i++)
    if (seen[i].node == n.id && seen[i].prop == p) return;
  seen[next % 128].node = n.id;
  seen[next % 128].prop = p;
  next++;
  cw_warn("set: '%s.%s' = %g is outside its range [%g, %g]: clamped to %g", cw_node_name(n), name, (double)asked, p->min, p->max,
          asked < p->min ? p->min : p->max);
}

bool cw_node_set(cw_node n, const char *name, cw_value v) {
  const cw_node_type *owner;
  const cw_prop *p = cw_node_find_prop(n, name, &owner);
  if (!p) {
    const cw_node_type *t = cw_node_get_type(n);
    cw_warn("set: '%s' (%s) has no property '%s'", cw_node_name(n), t ? t->name : "?", name);
    return false;
  }
  if (p->flags & CW_PROP_READONLY) {
    cw_warn("set: '%s.%s' is read-only", cw_node_name(n), name);
    return false;
  }
  if (p->type == CW_T_ENUM && v.type == CW_T_STRING && p->enum_names) { /* enum by name: "BOX" */
    for (int k = 0; p->enum_names[k]; k++)
      if (v.s && !strcmp(p->enum_names[k], v.s)) { v.type = CW_T_ENUM; v.i = k; break; }
    if (v.type != CW_T_ENUM) {
      char names[512] = "";
      for (int k = 0; p->enum_names[k]; k++) {
        strncat(names, k ? ", " : "", sizeof names - strlen(names) - 1);
        strncat(names, p->enum_names[k], sizeof names - strlen(names) - 1);
      }
      cw_warn("set: '%s.%s' has no enum value '%s' (one of: %s)", cw_node_name(n), name, v.s ? v.s : "", names);
      return false;
    }
  }
  if (!coerce(&v, p->type)) {
    cw_warn("set: '%s.%s' wants %s, got %s", cw_node_name(n), name, cw_type_name(p->type), cw_type_name(v.type));
    return false;
  }
  if (p->type == CW_T_FLOAT && (p->min != 0 || p->max != 0) && (v.f < p->min || v.f > p->max)) {
    warn_clamped(n, name, p, v.f);
    v.f = v.f < p->min ? (float)p->min : (float)p->max;
  }
  void *data = cw_node_data_t(n, owner);
  if (p->set) p->set(n, data, &v);
  else field_set(p, (uint8_t *)data + p->offset, &v);
  if (owner->prop_changed) owner->prop_changed(n, data, p);
  return true;
}

/* Direct access by descriptor for code that walks props itself (tree
 * snapshots): no name lookup, no coercion, no clamping, no warnings. */
void cw__prop_read(cw_node n, const cw_node_type *owner, const cw_prop *p, cw_value *out) {
  void *data = cw_node_data_t(n, owner);
  if (p->get) p->get(n, data, out);
  else field_get(p, (const uint8_t *)data + p->offset, out);
}
void cw__prop_write(cw_node n, const cw_node_type *owner, const cw_prop *p, const cw_value *v) {
  void *data = cw_node_data_t(n, owner);
  if (p->set) p->set(n, data, v);
  else field_set(p, (uint8_t *)data + p->offset, v);
  if (owner->prop_changed) owner->prop_changed(n, data, p);
}

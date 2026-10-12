/* .cwscene parsing, instancing and saving. */
#include <stdio.h>
#include <stdlib.h>

#include "scene_internal.h"
#include "cartwheel/platform.h"
#include "cartwheel/scene_file.h"
#include "cartwheel/vfs.h"

typedef struct { const char *key, *value; } kv; /* both interned */

typedef struct pnode {
  const char *name;
  const char *type;     /* NULL for instances and overrides */
  const char *parent;   /* NULL for the root, "." for its children, else a path */
  int instance;         /* ext_scene id, 0 = none */
  const char *groups;   /* comma separated, may be NULL */
  CW_ARRAY(kv) props;
} pnode;

typedef struct pconn { const char *signal, *from, *to, *method; int flags; } pconn;
typedef struct pext { int id; const char *path; } pext;
typedef struct psec { const cw_scene_section *sec; void *state; } psec;

struct cw_packed_scene {
  const char *path;
  CW_ARRAY(pnode) nodes;
  CW_ARRAY(pconn) conns;
  CW_ARRAY(pext) ext;
  CW_ARRAY(psec) secs; /* extension sections, in file order */
};

static CW_ARRAY(const cw_scene_section *) sections;

void cw_scene_register_section(const cw_scene_section *s) {
  for (int i = 0; i < sections.count; i++)
    if (!strcmp(sections.data[i]->name, s->name)) return;
  cw_array_push(&sections, s);
}

/* the header's name: "[track type=value]" -> "track" */
static const cw_scene_section *section_named(const char *header, size_t *len) {
  const char *h = header + 1;
  size_t n = 0;
  while (h[n] && h[n] != ' ' && h[n] != ']') n++;
  *len = n;
  for (int i = 0; i < sections.count; i++)
    if (strlen(sections.data[i]->name) == n && !strncmp(sections.data[i]->name, h, n)) return sections.data[i];
  return NULL;
}

static bool owns_sub(const cw_scene_section *s, const char *header) {
  if (!s || !s->subsections) return false;
  for (int i = 0; s->subsections[i]; i++) {
    size_t n = strlen(s->subsections[i]);
    if (!strncmp(header + 1, s->subsections[i], n) && (header[n + 1] == ' ' || header[n + 1] == ']')) return true;
  }
  return false;
}

static cw_map cache; /* interned path -> packed scene */

const char *cw_packed_scene_path(cw_packed_scene *ps) { return ps->path; }

/* ── parsing ─────────────────────────────────────────────────────────── */

/* attribute value after key= : quoted string or bare token */
static const char *attr(const char *line, const char *key) {
  size_t kl = strlen(key);
  for (const char *p = line; (p = strstr(p, key)); p += kl) {
    if (p != line && p[-1] != ' ' && p[-1] != '[') continue;
    if (p[kl] != '=') continue;
    const char *v = p + kl + 1;
    char buf[1024];
    int k = 0;
    if (*v == '"') {
      for (v++; *v && *v != '"' && k < 1023; v++) {
        if (*v == '\\' && v[1]) v++;
        buf[k++] = *v;
      }
    } else {
      for (; *v && *v != ' ' && *v != ']' && k < 1023; v++) buf[k++] = *v;
    }
    buf[k] = 0;
    return cw_intern(buf);
  }
  return NULL;
}

const char *cw_scene_attr(const char *header, const char *key) { return attr(header, key); }

bool cw_scene_node_in_instance(cw_node n, cw_node root) {
  for (; cw_node_valid(n) && n.id != root.id; n = cw_node_parent(n)) {
    node_rec *r = cw__node_rec(n);
    if (r->scene_root && r->scene_source) return true;
  }
  return false;
}

static void packed_free(cw_packed_scene *ps) {
  for (int i = 0; i < ps->secs.count; i++)
    if (ps->secs.data[i].sec->free) ps->secs.data[i].sec->free(ps->secs.data[i].state);
  cw_array_free(&ps->secs);
  for (int i = 0; i < ps->nodes.count; i++) cw_array_free(&ps->nodes.data[i].props);
  cw_array_free(&ps->nodes);
  cw_array_free(&ps->conns);
  cw_array_free(&ps->ext);
  cw_free(ps);
}

cw_packed_scene *cw_scene_parse(const char *text, const char *path) {
  cw_scene_register_builtin_types(); /* modules register their sections there */
  cw_packed_scene *ps = cw_calloc(1, sizeof *ps, CW_MEM_SCENE);
  ps->path = cw_intern(path ? path : "(memory)");
  pnode *cur = NULL;
  psec *ext = NULL; /* the open extension section */
  int lineno = 0;
  const char *p = text;
  while (*p) {
    const char *e = strchr(p, '\n');
    size_t len = e ? (size_t)(e - p) : strlen(p);
    lineno++;
    char line[4096];
    if (len >= sizeof line) len = sizeof line - 1;
    memcpy(line, p, len);
    line[len] = 0;
    if (len && line[len - 1] == '\r') line[--len] = 0;
    p += e ? len + 1 : len;
    char *s = line;
    while (*s == ' ' || *s == '\t') s++;
    if (!*s || *s == ';' || *s == '#') continue;
    if (*s == '[' && ext && owns_sub(ext->sec, s)) {
      if (!ext->sec->line(ext->state, s, ps->path, lineno)) { packed_free(ps); return NULL; }
      continue;
    }
    if (*s == '[') {
      ext = NULL;
      size_t hl;
      const cw_scene_section *sec = section_named(s, &hl);
      if (sec) {
        void *st = sec->begin(s, ps->path, lineno);
        if (!st) { packed_free(ps); return NULL; }
        cw_array_push(&ps->secs, ((psec){ sec, st }));
        ext = &ps->secs.data[ps->secs.count - 1];
        cur = NULL;
      } else if (!strncmp(s, "[scene", 6)) {
        cur = NULL;
      } else if (!strncmp(s, "[ext_scene", 10)) {
        pext x = { atoi(attr(s, "id") ? attr(s, "id") : "0"), attr(s, "path") };
        if (!x.id || !x.path) { cw_error("%s:%d: ext_scene needs id and path", ps->path, lineno); packed_free(ps); return NULL; }
        cw_array_push(&ps->ext, x);
        cur = NULL;
      } else if (!strncmp(s, "[node", 5)) {
        pnode n = { 0 };
        n.name = attr(s, "name");
        n.type = attr(s, "type");
        n.parent = attr(s, "parent");
        const char *inst = attr(s, "instance");
        n.instance = inst ? atoi(inst) : 0;
        n.groups = attr(s, "groups");
        if (!n.name) { cw_error("%s:%d: node needs a name", ps->path, lineno); packed_free(ps); return NULL; }
        if (!ps->nodes.count && n.parent) { cw_error("%s:%d: the first node is the root and has no parent", ps->path, lineno); packed_free(ps); return NULL; }
        if (ps->nodes.count && !n.parent) { cw_error("%s:%d: only the first node may omit parent", ps->path, lineno); packed_free(ps); return NULL; }
        if (!ps->nodes.count && !n.type && !n.instance) { cw_error("%s:%d: the root needs a type or instance", ps->path, lineno); packed_free(ps); return NULL; }
        cw_array_push(&ps->nodes, n);
        cur = &ps->nodes.data[ps->nodes.count - 1];
      } else if (!strncmp(s, "[connection", 11)) {
        pconn c = { attr(s, "signal"), attr(s, "from"), attr(s, "to"), attr(s, "method"), 0 };
        const char *fl = attr(s, "flags");
        c.flags = fl ? atoi(fl) : 0;
        if (!c.signal || !c.from || !c.to || !c.method) { cw_error("%s:%d: connection needs signal, from, to, method", ps->path, lineno); packed_free(ps); return NULL; }
        cw_array_push(&ps->conns, c);
        cur = NULL;
      } else {
        cw_error("%s:%d: unknown section %s", ps->path, lineno, s);
        packed_free(ps);
        return NULL;
      }
      continue;
    }
    if (ext) {
      if (!ext->sec->line(ext->state, s, ps->path, lineno)) { packed_free(ps); return NULL; }
      continue;
    }
    char *eq = strchr(s, '=');
    if (!eq || !cur) { cw_error("%s:%d: expected `name = value` inside a [node]", ps->path, lineno); packed_free(ps); return NULL; }
    char *k = s, *ke = eq;
    while (ke > k && (ke[-1] == ' ' || ke[-1] == '\t')) ke--;
    *ke = 0;
    char *v = eq + 1;
    while (*v == ' ' || *v == '\t') v++;
    kv item = { cw_intern(k), cw_intern(v) };
    cw_array_push(&cur->props, item);
  }
  if (!ps->nodes.count) { cw_error("%s: no nodes", ps->path); packed_free(ps); return NULL; }
  return ps;
}

cw_packed_scene *cw_scene_load(const char *asset_path) {
  if (!cache.slot_cap && !cache.count) cw_map_init(&cache, CW_MEM_SCENE);
  const char *key = cw_intern(asset_path);
  uint64_t v;
  if (cw_map_get(&cache, (uint64_t)(uintptr_t)key, &v)) return (cw_packed_scene *)(uintptr_t)v;
  size_t len;
  /* through the project layer (cartwheel/vfs.h): an editor's unsaved-to-disk
   * scenes and a native project directory come first, then the app's assets */
  char *text = cw_vfs_read(asset_path, &len);
  if (!text) { cw_error("scene '%s' not found in the project or the assets", asset_path); return NULL; }
  cw_packed_scene *ps = cw_scene_parse(text, asset_path);
  free(text);
  if (ps) cw_map_set(&cache, (uint64_t)(uintptr_t)key, (uint64_t)(uintptr_t)ps);
  return ps;
}

/* A re-read scene replaces the cached one. The old one is not freed: nodes
 * instanced from it may still point at it (instanced sub-scenes). */
void cw__scene_cache_put(const char *path, cw_packed_scene *ps) {
  if (!cache.slot_cap && !cache.count) cw_map_init(&cache, CW_MEM_SCENE);
  cw_map_set(&cache, (uint64_t)(uintptr_t)cw_intern(path), (uint64_t)(uintptr_t)ps);
}

/* ── instancing ──────────────────────────────────────────────────────── */

static const char *ext_path(cw_packed_scene *ps, int id) {
  for (int i = 0; i < ps->ext.count; i++)
    if (ps->ext.data[i].id == id) return ps->ext.data[i].path;
  return NULL;
}

/* node("path") values wait until the whole instance exists: the path is
 * relative to the scene root ("." = the root), like parent= */
typedef struct { cw_node n; const char *key, *path; } node_ref;
typedef CW_ARRAY(node_ref) node_refs;

static const char *node_ref_path(const char *text) {
  const char *q = !strncmp(text, "node(", 5) ? strchr(text, '"') : NULL;
  if (!q) return NULL;
  const char *e = strchr(q + 1, '"');
  return e ? cw_intern_n(q + 1, (size_t)(e - q - 1)) : NULL;
}

static void apply_props(cw_node n, pnode *pn, const char *scene_path, node_refs *refs) {
  for (int i = 0; i < pn->props.count; i++) {
    const cw_prop *prop = cw_node_find_prop(n, pn->props.data[i].key, NULL);
    if (!prop) {
      cw_warn("%s: node '%s' (%s) has no property '%s'", scene_path, pn->name, cw_node_get_type(n)->name, pn->props.data[i].key);
      continue;
    }
    cw_value v;
    const char *text = pn->props.data[i].value;
    if (prop->type == CW_T_NODE) {
      const char *path = node_ref_path(text);
      if (!path) cw_warn("%s: '%s.%s': cannot parse `%s` (a node value is node(\"path from the scene root\"))", scene_path,
                         pn->name, pn->props.data[i].key, text);
      else cw_array_push(refs, ((node_ref){ n, pn->props.data[i].key, path }));
      continue;
    }
    if (prop->type == CW_T_ENUM && prop->enum_names && ((text[0] >= 'A' && text[0] <= 'Z') || text[0] == '_')) {
      v = (cw_value){ CW_T_ENUM };
      for (int k = 0; prop->enum_names[k]; k++)
        if (!strcmp(prop->enum_names[k], text)) v.i = k;
    } else if (!cw_value_parse(text, prop->type, &v)) {
      cw_warn("%s: '%s.%s': cannot parse `%s`", scene_path, pn->name, pn->props.data[i].key, text);
      continue;
    }
    cw_node_set(n, pn->props.data[i].key, v);
  }
  if (pn->groups) {
    const char *g = pn->groups;
    while (*g) {
      const char *c = strchr(g, ',');
      size_t l = c ? (size_t)(c - g) : strlen(g);
      if (l) cw_node_add_group(n, cw_intern_n(g, l));
      g += l + (c ? 1 : 0);
    }
  }
}

void cw_scene_cache_forget(const char *asset_path) {
  if (!cache.slot_cap && !cache.count) return;
  if (!asset_path) {
    cw_map_free(&cache);
    cw_map_init(&cache, CW_MEM_SCENE);
    return;
  }
  /* the packed scene itself stays alive: nodes instanced from it point at it */
  cw_map_remove(&cache, (uint64_t)(uintptr_t)cw_intern(asset_path));
}

cw_node cw_scene_instance(cw_packed_scene *ps) {
  if (!ps) return (cw_node){ 0 };
  cw_scene_register_builtin_types();
  cw_node root = { 0 };
  node_refs refs = { 0 };
  for (int i = 0; i < ps->nodes.count; i++) {
    pnode *pn = &ps->nodes.data[i];
    cw_node parent = { 0 };
    if (i > 0) {
      parent = strcmp(pn->parent, ".") ? cw_node_get(root, pn->parent) : root;
      if (CW_HANDLE_IS_NULL(parent)) {
        cw_error("%s: node '%s': parent '%s' not found", ps->path, pn->name, pn->parent);
        continue;
      }
    }
    cw_node n = { 0 };
    if (pn->instance) {
      const char *sub = ext_path(ps, pn->instance);
      n = sub ? cw_scene_instance_path(sub) : (cw_node){ 0 };
      if (CW_HANDLE_IS_NULL(n)) { cw_error("%s: node '%s': instance %d did not load", ps->path, pn->name, pn->instance); continue; }
      cw_node_set_name(n, pn->name);
      node_rec *r = cw__node_rec(n);
      r->scene_root = true;
      r->scene_source = (uint64_t)(uintptr_t)cw_scene_load(sub);
    } else if (pn->type) {
      n = cw_node_new(pn->type, pn->name);
      if (CW_HANDLE_IS_NULL(n)) {
        cw_error("%s: node '%s' (type %s) and its children were not created: the scene file itself is unchanged", ps->path,
                 pn->name, pn->type);
        continue;
      }
    } else {
      /* override: an existing node of an instanced subtree */
      char path[1024];
      snprintf(path, sizeof path, "%s%s%s", strcmp(pn->parent, ".") ? pn->parent : "", strcmp(pn->parent, ".") ? "/" : "", pn->name);
      n = cw_node_get(root, path);
      if (CW_HANDLE_IS_NULL(n)) { cw_error("%s: override target '%s' does not exist", ps->path, path); continue; }
      apply_props(n, pn, ps->path, &refs);
      continue;
    }
    apply_props(n, pn, ps->path, &refs);
    if (i == 0) root = n;
    else cw_node_add_child(parent, n);
  }
  for (int i = 0; i < ps->conns.count; i++) {
    pconn *c = &ps->conns.data[i];
    cw_node from = strcmp(c->from, ".") ? cw_node_get(root, c->from) : root;
    cw_node to = strcmp(c->to, ".") ? cw_node_get(root, c->to) : root;
    if (CW_HANDLE_IS_NULL(from) || CW_HANDLE_IS_NULL(to)) {
      cw_error("%s: connection %s: '%s' -> '%s' not found", ps->path, c->signal, c->from, c->to);
      continue;
    }
    cw_connect(from, c->signal, to, c->method, NULL, NULL, (uint32_t)c->flags);
  }
  for (int i = 0; i < refs.count; i++) {
    node_ref *r = &refs.data[i];
    if (!cw_node_valid(r->n)) continue;
    /* "" = none; "." = the scene root; "/root/..." (older files) = the
     * current tree, as cw_value_parse does; else a path from the root */
    cw_node target = !r->path[0] ? (cw_node){ 0 } : !strcmp(r->path, ".") ? root : cw_node_get(root, r->path);
    if (r->path[0] && CW_HANDLE_IS_NULL(target))
      cw_warn("%s: '%s.%s': node \"%s\" not found", ps->path, cw_node_name(r->n), r->key, r->path);
    cw_value v = { CW_T_NODE };
    v.node = target;
    cw_node_set(r->n, r->key, v);
  }
  cw_array_free(&refs);
  for (int i = 0; i < ps->secs.count; i++)
    if (ps->secs.data[i].sec->instance) ps->secs.data[i].sec->instance(ps->secs.data[i].state, root, ps->path);
  return root;
}

cw_node cw_scene_instance_path(const char *asset_path) { return cw_scene_instance(cw_scene_load(asset_path)); }

cw_node cw_scene_instance_linked(const char *asset_path) {
  cw_packed_scene *ps = cw_scene_load(asset_path);
  cw_node n = cw_scene_instance(ps);
  if (CW_HANDLE_IS_NULL(n)) return n;
  node_rec *r = cw__node_rec(n);
  r->scene_root = true;
  r->scene_source = (uint64_t)(uintptr_t)ps;
  return n;
}

const char *cw_scene_instance_source(cw_node n) {
  node_rec *r = cw_node_valid(n) ? cw__node_rec(n) : NULL;
  if (!r || !r->scene_root || !r->scene_source) return NULL;
  return ((cw_packed_scene *)(uintptr_t)r->scene_source)->path;
}

void cw_packed_scene_free(cw_packed_scene *ps) {
  if (!ps) return;
  uint64_t v;
  if (cache.slot_cap && cw_map_get(&cache, (uint64_t)(uintptr_t)ps->path, &v) && (cw_packed_scene *)(uintptr_t)v == ps) {
    cw_error("cw_packed_scene_free: '%s' is in the load cache (cw_scene_load); only free scenes from cw_scene_parse", ps->path);
    return;
  }
  packed_free(ps);
}

/* ── saving ──────────────────────────────────────────────────────────── */

typedef struct { char *buf; size_t len, cap; } sbuf;
static void sb_put(sbuf *b, const char *s) {
  size_t n = strlen(s);
  if (b->len + n + 1 > b->cap) {
    b->cap = (b->len + n + 1) * 2 + 256;
    b->buf = cw_realloc(b->buf, b->cap, CW_MEM_SCENE);
  }
  memcpy(b->buf + b->len, s, n + 1);
  b->len += n;
}

static void sb_put_ctx(void *b, const char *s) { sb_put(b, s); }

static bool quote_needed(const char *s) { return true; (void)s; }

static void put_attr(sbuf *b, const char *k, const char *v) {
  char tmp[1200];
  snprintf(tmp, sizeof tmp, " %s=%s%s%s", k, quote_needed(v) ? "\"" : "", v, quote_needed(v) ? "\"" : "");
  sb_put(b, tmp);
}

/* Format a property for comparison/writing; enums by name. */
static void prop_text(cw_node n, const cw_prop *p, char *out, int cap) {
  cw_value v;
  if (!cw_node_get_prop(n, p->name, &v)) { out[0] = 0; return; }
  if (p->type == CW_T_ENUM && p->enum_names) {
    int k = 0;
    while (p->enum_names[k] && k < v.i) k++;
    if (p->enum_names[k] && k == v.i) { snprintf(out, (size_t)cap, "%s", p->enum_names[k]); return; }
  }
  cw_value_format(&v, out, cap);
}

typedef struct save_ctx {
  sbuf *b;
  cw_node root;
  CW_ARRAY(const char *) ext_paths;
} save_ctx;

static int ext_id(save_ctx *c, const char *path) {
  for (int i = 0; i < c->ext_paths.count; i++)
    if (c->ext_paths.data[i] == path) return i + 1;
  cw_array_push(&c->ext_paths, path);
  return c->ext_paths.count;
}

/* A node-valued property as scene text: node("path from base"), "." for
 * base itself, "" for none. A node outside base's subtree keeps its absolute
 * path (it resolves only when loaded into that same tree). */
static void node_text(cw_node base, cw_node n, const cw_prop *p, char *out, int cap) {
  cw_value v;
  char path[1024] = "";
  if (!CW_HANDLE_IS_NULL(n) && cw_node_get_prop(n, p->name, &v) && cw_node_valid(v.node)) {
    bool inside = false;
    for (cw_node a = v.node; !CW_HANDLE_IS_NULL(a) && !inside; a = cw_node_parent(a)) inside = a.id == base.id;
    if (inside) {
      cw_node_path(v.node, base, path, sizeof path);
      if (!path[0]) snprintf(path, sizeof path, ".");
    } else if (cw_node_path(v.node, (cw_node){ 0 }, path, sizeof path) < 0) path[0] = 0;
  }
  snprintf(out, (size_t)cap, "node(\"%s\")", path);
}
static cw_node top_of(cw_node n) {
  cw_node t = n;
  for (cw_node a = n; !CW_HANDLE_IS_NULL(a); a = cw_node_parent(a)) t = a;
  return t;
}
/* the instance root an override node belongs to (its nearest instanced ancestor) */
static cw_node instance_root_of(save_ctx *c, cw_node n) {
  for (cw_node a = n; !CW_HANDLE_IS_NULL(a) && a.id != c->root.id; a = cw_node_parent(a)) {
    node_rec *r = cw__node_rec(a);
    if (r && r->scene_root && r->scene_source) return a;
  }
  return c->root;
}

/* reference node for defaults: a fresh node of the same type, or for an
 * instanced subtree the corresponding node of a fresh instance */
static void write_node(save_ctx *c, cw_node n, cw_node ref, bool is_override, sbuf *body, int *ext_used) {
  (void)ext_used;
  char path[1024], tmp[4096];
  node_rec *r = cw__node_rec(n);
  sbuf props = { 0 };
  const cw_prop *list[256];
  int np = cw_node_props(n, list, NULL, 256);
  for (int i = 0; i < np && i < 256; i++) {
    const cw_prop *p = list[i];
    if (!(p->flags & CW_PROP_STORED) || (p->flags & CW_PROP_READONLY)) continue;
    char a[2048], d[2048];
    if (p->type == CW_T_NODE) {
      /* compared as paths from each side's own scene root (an override's
       * instance vs the fresh instance), written from this scene's root */
      node_text(instance_root_of(c, n), n, p, a, sizeof a);
      node_text(top_of(ref), ref, p, d, sizeof d);
      if (strcmp(a, d)) node_text(c->root, n, p, a, sizeof a);
    } else {
      prop_text(n, p, a, sizeof a);
      prop_text(ref, p, d, sizeof d);
    }
    if (!strcmp(a, d)) continue;
    snprintf(tmp, sizeof tmp, "%s = %s\n", p->name, a);
    sb_put(&props, tmp);
  }
  /* groups: written when the node has any its reference lacks (inside an
   * instance the reference is the instanced scene's node, which already
   * carries that scene's groups: repeating them would not round-trip) */
  bool groups_differ = false;
  for (int i = 0; i < r->groups.count && !groups_differ; i++)
    groups_differ = CW_HANDLE_IS_NULL(ref) || !cw_node_in_group(ref, r->groups.data[i]);
  if (is_override && !props.len && !groups_differ) { cw_free(props.buf); return; }
  sb_put(body, "\n[node");
  put_attr(body, "name", r->name);
  if (n.id != c->root.id) {
    cw_node_path(r->parent, c->root, path, sizeof path);
    put_attr(body, "parent", path[0] ? path : ".");
  }
  if (!is_override) {
    if (r->scene_root && r->scene_source) {
      cw_packed_scene *src = (cw_packed_scene *)(uintptr_t)r->scene_source;
      snprintf(tmp, sizeof tmp, "%d", ext_id(c, src->path));
      sb_put(body, " instance=");
      sb_put(body, tmp);
    } else {
      put_attr(body, "type", r->type->name);
    }
  }
  if (groups_differ) {
    char g[1024] = "";
    for (int i = 0; i < r->groups.count; i++) {
      if (i) strncat(g, ",", sizeof g - strlen(g) - 1);
      strncat(g, r->groups.data[i], sizeof g - strlen(g) - 1);
    }
    put_attr(body, "groups", g);
  }
  sb_put(body, "]\n");
  if (props.len) sb_put(body, props.buf);
  cw_free(props.buf);
}

static void save_rec(save_ctx *c, cw_node n, cw_node ref, bool inside_instance, sbuf *body) {
  node_rec *r = cw__node_rec(n);
  cw_node fresh = { 0 };
  /* an instance the scene itself placed; a nested instance INSIDE an instanced
   * scene is that scene's node (an override at most), not a new instance */
  bool instance_root = r->scene_root && r->scene_source && n.id != c->root.id && !inside_instance;
  if (instance_root) {
    fresh = cw_scene_instance((cw_packed_scene *)(uintptr_t)r->scene_source);
    ref = fresh;
  } else if (!inside_instance) {
    fresh = cw_node_new(r->type->name, NULL);
    ref = fresh;
  }
  write_node(c, n, ref, inside_instance && !instance_root, body, NULL);
  for (int i = 0; i < r->children.count; i++) {
    cw_node ch = r->children.data[i];
    bool child_inside = inside_instance || instance_root;
    cw_node child_ref = { 0 };
    if (child_inside) {
      child_ref = cw_node_get(ref, cw_node_name(ch));
      if (CW_HANDLE_IS_NULL(child_ref)) child_inside = false; /* added after instancing: a plain node */
    }
    save_rec(c, ch, child_ref, child_inside, body);
  }
  if (!CW_HANDLE_IS_NULL(fresh)) cw_node_free(fresh);
}

/* connections: every one whose emitter and target are both under root */
int cw__signals_of(cw_node emitter, uint64_t *out, int cap);
bool cw__signal_info(uint64_t id, cw_node *emitter, const char **signal, cw_node *target, const char **method, uint32_t *flags);

static bool under(cw_node n, cw_node root) {
  for (; !CW_HANDLE_IS_NULL(n); n = cw_node_parent(n))
    if (n.id == root.id) return true;
  return false;
}

/* A connection an instanced sub-scene already makes (its own [connection]
 * between its own nodes) belongs to that scene's file: writing it again in
 * the parent scene would connect it twice on the next load. */
static bool inherited_conn(save_ctx *c, cw_node em, cw_node tg, const char *sig, const char *method) {
  for (cw_node r = em; !CW_HANDLE_IS_NULL(r) && r.id != c->root.id; r = cw_node_parent(r)) {
    node_rec *rr = cw__node_rec(r);
    if (!rr || !rr->scene_root || !rr->scene_source || !under(tg, r)) continue;
    cw_packed_scene *ps = (cw_packed_scene *)(uintptr_t)rr->scene_source;
    char from[1024], to[1024];
    cw_node_path(em, r, from, sizeof from);
    cw_node_path(tg, r, to, sizeof to);
    for (int i = 0; i < ps->conns.count; i++) {
      pconn *k = &ps->conns.data[i];
      if (!strcmp(k->signal, sig) && !strcmp(k->method, method) && !strcmp(k->from, from[0] ? from : ".") &&
          !strcmp(k->to, to[0] ? to : "."))
        return true;
    }
  }
  return false;
}

typedef struct { save_ctx *c; sbuf *b; } conn_walk;
static bool conn_fn(cw_node n, void *u) {
  conn_walk *w = u;
  uint64_t ids[64];
  int k = cw__signals_of(n, ids, 64);
  for (int i = 0; i < k; i++) {
    cw_node em, tg;
    const char *sig, *method;
    uint32_t flags;
    if (!cw__signal_info(ids[i], &em, &sig, &tg, &method, &flags) || !method || !under(tg, w->c->root)) continue;
    if (inherited_conn(w->c, em, tg, sig, method)) continue;
    char from[1024], to[1024], tmp[3000];
    cw_node_path(em, w->c->root, from, sizeof from);
    cw_node_path(tg, w->c->root, to, sizeof to);
    snprintf(tmp, sizeof tmp, "\n[connection signal=\"%s\" from=\"%s\" to=\"%s\" method=\"%s\"%s]\n", sig,
             from[0] ? from : ".", to[0] ? to : ".", method, flags ? (flags == 1 ? " flags=1" : flags == 2 ? " flags=2" : " flags=3") : "");
    sb_put(w->b, tmp);
  }
  return true;
}

char *cw_scene_save(cw_node root) {
  save_ctx c = { 0 };
  c.root = root;
  sbuf body = { 0 };
  save_rec(&c, root, (cw_node){ 0 }, false, &body);
  for (int i = 0; i < sections.count; i++)
    if (sections.data[i]->save) sections.data[i]->save(root, sb_put_ctx, &body);
  conn_walk w = { &c, &body };
  cw_node_walk(root, conn_fn, &w);
  sbuf out = { 0 };
  sb_put(&out, "[scene format=1]\n");
  for (int i = 0; i < c.ext_paths.count; i++) {
    char tmp[1200];
    snprintf(tmp, sizeof tmp, "[ext_scene id=%d path=\"%s\"]\n", i + 1, c.ext_paths.data[i]);
    sb_put(&out, tmp);
  }
  if (body.buf) sb_put(&out, body.buf);
  cw_free(body.buf);
  cw_array_free(&c.ext_paths);
  return out.buf;
}

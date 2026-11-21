/* Surfaces of the game's own (cartwheel/surface.h): the registry, each
 * material's values, the fill hook that sends them with a draw, and the node
 * type a surface can be. The generator side is tools/lib/surface-part.mjs,
 * which turns a module's `params` and `surface()` into a shading part. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cartwheel/core.h"
#include "cartwheel/resource.h"
#include "cartwheel/scene.h"
#include "cartwheel/shading.h"
#include "cartwheel/surface.h"
#include "pipeline_internal.h"

enum { MAX_PARAMS = 32, MAX_COUNT = 32, NAME_MAX_ = 11 };

typedef struct sparam {
  char name[32];
  char uname[48]; /* "<surface>_<param>": the uniform or texture after "ext." (its address is three.c's lookup key) */
  cw_surface_type type;
  int n;   /* floats (0 for a texture) */
  int off; /* into a value block; a texture's index into the texture list */
  int count; /* elements of an array (1 otherwise) */
  char (*enames)[48]; /* an array: each element's uniform, "<surface>_<param>_<i>" (a program holds each as a vec4 of its own) */
} sparam;

typedef struct srec {
  t3_material *m; /* retained */
  float *vals;
  uint32_t set; /* bit per parameter: the material has its own value */
  t3_texture **tex; /* retained, NULL = the surface's */
} srec;

typedef struct surface {
  char name[NAME_MAX_ + 1];
  char inv_name[48]; /* "<surface>_objinv" */
  sparam p[MAX_PARAMS];
  int np, nfloats, ntex;
  bool moves;
  float *defaults, *globals;
  uint32_t gset;
  t3_texture **gtex; /* retained */
  CW_ARRAY(srec) recs;
  cw_map index_of; /* material -> index + 1 */
  cw_node_type *node_type;
  cw_prop *node_props;
  char said[256]; /* the last complaint (each said once in a row) */
} surface;

static surface **S;
static int nS, capS;
static t3_texture *white;

static surface *find_surface(const char *name) {
  for (int i = 0; name && i < nS; i++)
    if (!strcmp(S[i]->name, name)) return S[i];
  return NULL;
}
static int find_param(const surface *s, const char *name) {
  for (int i = 0; name && i < s->np; i++)
    if (!strcmp(s->p[i].name, name)) return i;
  return -1;
}
static int find_rec(const surface *s, const t3_material *m) {
  uint64_t v;
  if (!m || !cw_map_get(&s->index_of, (uint64_t)(uintptr_t)m, &v)) return -1;
  return (int)v - 1;
}
static void complain(surface *s, const char *fmt, const char *a, const char *b) {
  char msg[256];
  snprintf(msg, sizeof msg, fmt, a ? a : "", b ? b : "");
  if (s && !strcmp(s->said, msg)) return;
  if (s) snprintf(s->said, sizeof s->said, "%s", msg);
  cw_error("%s", msg);
}

static t3_texture *white_texture(void) {
  if (!white) {
    static const uint8_t px[16] = { 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255 };
    white = t3_texture_new(2, 2, px);
    white->wrap_s = white->wrap_t = T3_REPEAT;
  }
  return white;
}

/* ── the fill hook ────────────────────────────────────────────────────── */
static void fill(t3_extension_ctx *c, void *u) {
  surface *s = u;
  int ri = find_rec(s, t3_extension_material(c));
  const srec *r = ri >= 0 ? &s->recs.data[ri] : NULL; /* (a material copied with the part on it draws the surface's values) */
  for (int i = 0; i < s->np; i++) {
    const sparam *p = &s->p[i];
    bool own = r && (r->set >> i & 1);
    if (p->type == CW_SURFACE_TEXTURE) {
      t3_texture *t = r && r->tex[p->off] ? r->tex[p->off] : s->gtex[p->off] ? s->gtex[p->off] : white_texture();
      t3_extension_set_texture(c, p->uname, t);
    } else {
      const float *v = own ? r->vals + p->off : (s->gset >> i & 1) ? s->globals + p->off : s->defaults + p->off;
      if (p->count > 1) {
        for (int k = 0; k < p->count; k++) t3_extension_set_uniform(c, p->enames[k], v + 4 * k, 4);
      } else {
        t3_extension_set_uniform(c, p->uname, v, p->n);
      }
    }
  }
  if (s->moves) {
    /* world to the mesh's own space, for a displacement given in metres (surface-part.mjs toLocal; an instanced
     * program does not have it) */
    const t3_object *o = t3_extension_object(c);
    t3_mat4 inv;
    t3_mat4_invert(&inv, &o->matrix_world);
    /* (an instanced program has none: its instances are placed in the mesh's space already; tools/lib/surface-part.mjs) */
    cw__ext_miss_quiet = true;
    t3_extension_set_uniform(c, s->inv_name, inv.e, 16);
    cw__ext_miss_quiet = false;
  }
}

/* ── registration ─────────────────────────────────────────────────────── */
static bool ident_ok(const char *s, bool lower_only, size_t max) {
  if (!s || !*s || strlen(s) > max) return false;
  for (const char *c = s; *c; c++)
    if (!((*c >= 'a' && *c <= 'z') || (*c >= '0' && *c <= '9') || (!lower_only && *c >= 'A' && *c <= 'Z'))) return false;
  return true;
}

static void node_type_make(surface *s, const char *type_name);

bool cw_surface_register(const cw_surface_desc *d) {
  if (!d || !ident_ok(d->name, true, NAME_MAX_ - 1)) {
    cw_error("cw_surface_register: '%s' is not a surface name ([a-z0-9], at most %d characters)", d && d->name ? d->name : "(null)", NAME_MAX_ - 1);
    return false;
  }
  if (d->param_count < 0 || d->param_count > MAX_PARAMS || (d->param_count && !d->params)) {
    cw_error("cw_surface_register: surface %s: %d parameters (at most %d)", d->name, d->param_count, MAX_PARAMS);
    return false;
  }
  for (int i = 0; i < d->param_count; i++) {
    const cw_surface_param *p = &d->params[i];
    bool ok = ident_ok(p->name, false, 31) && p->type >= CW_SURFACE_FLOAT && p->type <= CW_SURFACE_TEXTURE && p->count >= 0 &&
              p->count <= MAX_COUNT && (p->count <= 1 || p->type == CW_SURFACE_VEC4);
    for (int j = 0; ok && j < i; j++) ok = strcmp(p->name, d->params[j].name) != 0;
    if (!ok) {
      cw_error("cw_surface_register: surface %s: parameter %d ('%s') is not valid (a name of [a-zA-Z0-9] used once; count only on a "
               "VEC4, at most %d)", d->name, i, p->name ? p->name : "(null)", MAX_COUNT);
      return false;
    }
  }
  surface *s = find_surface(d->name);
  bool fresh = !s;
  if (fresh) {
    s = cw_calloc(1, sizeof *s, CW_MEM_RENDER);
    snprintf(s->name, sizeof s->name, "%s", d->name);
    snprintf(s->inv_name, sizeof s->inv_name, "%s_objinv", d->name);
    cw_map_init(&s->index_of, CW_MEM_RENDER);
  } else if (s->recs.count) {
    /* materials hold value blocks of the old layout: only the defaults may change */
    bool same = s->np == d->param_count;
    for (int i = 0; same && i < s->np; i++)
      same = !strcmp(s->p[i].name, d->params[i].name) && s->p[i].type == d->params[i].type;
    if (!same) {
      cw_error("cw_surface_register: surface %s is on %d material(s): its parameter list cannot change now", d->name, s->recs.count);
      return false;
    }
  }
  s->np = d->param_count;
  s->nfloats = s->ntex = 0;
  s->moves = d->moves_vertices;
  for (int i = 0; i < s->np; i++) {
    const cw_surface_param *p = &d->params[i];
    sparam *q = &s->p[i];
    snprintf(q->name, sizeof q->name, "%s", p->name);
    snprintf(q->uname, sizeof q->uname, "%s_%s", s->name, p->name);
    q->type = p->type;
    static const int width[] = { 1, 2, 3, 4, 4, 16, 0 };
    q->count = p->type == CW_SURFACE_VEC4 && p->count > 1 ? p->count : 1;
    q->n = width[p->type] * q->count;
    if (q->count > 1 && !q->enames) {
      q->enames = cw_calloc((size_t)q->count, sizeof *q->enames, CW_MEM_RENDER);
      for (int k = 0; k < q->count; k++) snprintf(q->enames[k], sizeof q->enames[k], "%s_%s_%d", s->name, p->name, k);
    }
    q->off = p->type == CW_SURFACE_TEXTURE ? s->ntex++ : s->nfloats;
    s->nfloats += q->n;
  }
  s->defaults = cw_realloc(s->defaults, (size_t)CW_MAX(s->nfloats, 1) * sizeof(float), CW_MEM_RENDER);
  if (fresh) {
    s->globals = cw_calloc((size_t)CW_MAX(s->nfloats, 1), sizeof(float), CW_MEM_RENDER);
    s->gtex = cw_calloc((size_t)CW_MAX(s->ntex, 1), sizeof *s->gtex, CW_MEM_RENDER);
  }
  for (int i = 0; i < s->np; i++) {
    const cw_surface_param *p = &d->params[i];
    const sparam *q = &s->p[i];
    float *v = s->defaults + q->off;
    if (p->type == CW_SURFACE_TEXTURE) continue;
    if (p->type == CW_SURFACE_MAT4) {
      for (int k = 0; k < 16; k++) v[k] = k % 5 == 0;
    } else {
      int w = p->type == CW_SURFACE_COLOR ? 4 : p->type == CW_SURFACE_VEC4 ? 4 : (int)p->type + 1;
      for (int k = 0; k < q->n; k++) v[k] = p->value[k % w];
    }
  }
  if (fresh) {
    if (nS == capS) {
      capS = capS ? capS * 2 : 8;
      S = cw_realloc(S, (size_t)capS * sizeof *S, CW_MEM_RENDER);
    }
    S[nS++] = s;
  }
  cw_shading_part_traits(s->name, CW_PART_SURFACE | (s->moves ? CW_PART_MOVES_VERTICES : 0));
  /* a batch shares its material, and the one value of the object (the inverse world matrix) is not in an
   * instanced program: meshes with a surface may be auto-instanced */
  cw_shading_part_everywhere(s->name, fill, s, true);
  if (fresh && d->node_type && *d->node_type) node_type_make(s, d->node_type);
  return true;
}

bool cw_surface_known(const char *name) { return find_surface(name) != NULL; }

/* ── materials ────────────────────────────────────────────────────────── */
bool cw_surface_apply(t3_material *m, const char *name) {
  surface *s = find_surface(name);
  if (!m) return false;
  if (!s) {
    complain(NULL, "cw_surface_apply: no surface '%s' (cw_surface_register)%s", name, "");
    return false;
  }
  if (m->type != T3_MESH_STANDARD_MATERIAL && m->type != T3_MESH_PHYSICAL_MATERIAL && m->type != T3_MESH_LAMBERT_MATERIAL &&
      m->type != T3_MESH_PHONG_MATERIAL) {
    complain(s, "cw_surface_apply: surface %s: material '%s' is not a Standard, Physical, Lambert or Phong material", name, m->name);
    return false;
  }
  if (find_rec(s, m) >= 0) return true;
  if (!cw_shading_add(m, s->name)) return false;
  srec r = { .m = t3_retain(m) };
  r.vals = cw_calloc((size_t)CW_MAX(s->nfloats, 1), sizeof(float), CW_MEM_RENDER);
  r.tex = cw_calloc((size_t)CW_MAX(s->ntex, 1), sizeof *r.tex, CW_MEM_RENDER);
  cw_array_push(&s->recs, r);
  cw_map_set(&s->index_of, (uint64_t)(uintptr_t)m, (uint64_t)s->recs.count);
  return true;
}

static void rec_free(surface *s, srec *r) {
  for (int i = 0; i < s->ntex; i++)
    if (r->tex[i]) t3_release(r->tex[i]);
  cw_free(r->tex);
  cw_free(r->vals);
  t3_release(r->m);
}

void cw_surface_remove(t3_material *m, const char *name) {
  surface *s = find_surface(name);
  int i = s ? find_rec(s, m) : -1;
  if (i < 0) return;
  cw_shading_remove(m, s->name);
  cw_map_remove(&s->index_of, (uint64_t)(uintptr_t)m);
  srec gone = s->recs.data[i];
  int last = s->recs.count - 1;
  if (i != last) {
    s->recs.data[i] = s->recs.data[last];
    cw_map_set(&s->index_of, (uint64_t)(uintptr_t)s->recs.data[i].m, (uint64_t)i + 1);
  }
  s->recs.count--;
  rec_free(s, &gone);
}

bool cw_surface_has(const t3_material *m, const char *name) {
  const surface *s = find_surface(name);
  return s && find_rec(s, m) >= 0;
}

int cw_surface_material_count(const char *name) {
  const surface *s = find_surface(name);
  return s ? s->recs.count : 0;
}

typedef struct { const char *name; int n; CW_ARRAY(t3_material *) seen; } tree_ctx;
static void tree_visit(t3_object *o, void *u) {
  tree_ctx *c = u;
  if (o->type != T3_MESH && o->type != T3_SKINNED_MESH && o->type != T3_INSTANCED_MESH) return;
  t3_mesh *m = (t3_mesh *)o;
  for (int k = 0; k < m->material_count; k++) {
    t3_material *mt = m->materials[k];
    if (!mt || cw_shading_has(mt, "meshdecal")) continue; /* (a decal clipped onto the mesh keeps its own look) */
    bool dup = false;
    for (int j = 0; j < c->seen.count; j++) dup |= c->seen.data[j] == mt;
    if (dup) continue;
    cw_array_push(&c->seen, mt);
    if (cw_surface_apply(mt, c->name)) c->n++;
  }
}
int cw_surface_apply_tree(t3_object *root, const char *name) {
  if (!root) return 0;
  tree_ctx c = { name, 0, { 0 } };
  t3_object_traverse(root, tree_visit, &c);
  cw_array_free(&c.seen);
  return c.n;
}

/* the parameter and the material's record, or why not (said once) */
static bool locate(const char *fn, const t3_material *m, const char *name, const char *param, surface **s, int *pi, int *ri) {
  *s = find_surface(name);
  if (!*s) {
    complain(NULL, "%s: no surface '%s' (cw_surface_register)", fn, name);
    return false;
  }
  *pi = find_param(*s, param);
  if (*pi < 0) {
    char what[96];
    snprintf(what, sizeof what, "%s: surface %s", fn, name);
    complain(*s, "%s has no parameter '%s'", what, param);
    return false;
  }
  if (!ri) return true;
  *ri = find_rec(*s, m);
  if (*ri < 0) {
    char what[96];
    snprintf(what, sizeof what, "%s: surface %s", fn, name);
    complain(*s, "%s is not on material '%s' (cw_surface_apply)", what, m && m->name ? m->name : "");
    return false;
  }
  return true;
}
static bool width_ok(const char *fn, surface *s, const sparam *p, int n) {
  if (p->type != CW_SURFACE_TEXTURE && n == p->n) return true;
  char what[128];
  snprintf(what, sizeof what, "%s: surface %s parameter %s takes %d float(s), not %d", fn, s->name, p->name, p->n, n);
  complain(s, "%s%s", what, p->type == CW_SURFACE_TEXTURE ? " (a texture: cw_surface_set_texture)" : "");
  return false;
}

bool cw_surface_set(t3_material *m, const char *name, const char *param, const float *v, int n) {
  surface *s;
  int pi, ri;
  if (!v || !locate("cw_surface_set", m, name, param, &s, &pi, &ri) || !width_ok("cw_surface_set", s, &s->p[pi], n)) return false;
  srec *r = &s->recs.data[ri];
  memcpy(r->vals + s->p[pi].off, v, (size_t)n * sizeof(float));
  r->set |= 1u << pi;
  return true;
}
bool cw_surface_set_float(t3_material *m, const char *name, const char *param, float v) { return cw_surface_set(m, name, param, &v, 1); }

bool cw_surface_set_at(t3_material *m, const char *name, const char *param, int index, const float v[4]) {
  surface *s;
  int pi, ri;
  if (!v || !locate("cw_surface_set_at", m, name, param, &s, &pi, &ri)) return false;
  const sparam *p = &s->p[pi];
  if (p->type != CW_SURFACE_VEC4 || index < 0 || index * 4 >= p->n) {
    complain(s, "cw_surface_set_at: surface %s parameter %s has no such element", name, param);
    return false;
  }
  srec *r = &s->recs.data[ri];
  /* the first element set: the rest start from what the material drew with */
  if (!(r->set >> pi & 1)) memcpy(r->vals + p->off, ((s->gset >> pi & 1) ? s->globals : s->defaults) + p->off, (size_t)p->n * sizeof(float));
  memcpy(r->vals + p->off + index * 4, v, 4 * sizeof(float));
  r->set |= 1u << pi;
  return true;
}

static bool texture_param(const char *fn, surface *s, int pi) {
  if (s->p[pi].type == CW_SURFACE_TEXTURE) return true;
  complain(s, "%s: surface parameter %s is not a texture", fn, s->p[pi].uname);
  return false;
}
bool cw_surface_set_texture(t3_material *m, const char *name, const char *param, t3_texture *t) {
  surface *s;
  int pi, ri;
  if (!locate("cw_surface_set_texture", m, name, param, &s, &pi, &ri) || !texture_param("cw_surface_set_texture", s, pi)) return false;
  t3_texture **slot = &s->recs.data[ri].tex[s->p[pi].off];
  if (t) t3_retain(t);
  if (*slot) t3_release(*slot);
  *slot = t;
  return true;
}

int cw_surface_get(const t3_material *m, const char *name, const char *param, float *out, int cap) {
  surface *s = find_surface(name);
  int pi = s ? find_param(s, param) : -1;
  if (pi < 0 || s->p[pi].type == CW_SURFACE_TEXTURE) return 0;
  const sparam *p = &s->p[pi];
  int ri = find_rec(s, m);
  const float *v = ri >= 0 && (s->recs.data[ri].set >> pi & 1) ? s->recs.data[ri].vals + p->off
                   : (s->gset >> pi & 1) ? s->globals + p->off : s->defaults + p->off;
  if (out) memcpy(out, v, (size_t)CW_MIN(cap, p->n) * sizeof(float));
  return p->n;
}

void cw_surface_unset(t3_material *m, const char *name, const char *param) {
  surface *s = find_surface(name);
  int pi = s ? find_param(s, param) : -1, ri = s ? find_rec(s, m) : -1;
  if (pi < 0 || ri < 0) return;
  srec *r = &s->recs.data[ri];
  r->set &= ~(1u << pi);
  if (s->p[pi].type == CW_SURFACE_TEXTURE && r->tex[s->p[pi].off]) t3_release(r->tex[s->p[pi].off]), r->tex[s->p[pi].off] = NULL;
}

bool cw_surface_set_global(const char *name, const char *param, const float *v, int n) {
  surface *s;
  int pi;
  if (!v || !locate("cw_surface_set_global", NULL, name, param, &s, &pi, NULL) || !width_ok("cw_surface_set_global", s, &s->p[pi], n)) return false;
  memcpy(s->globals + s->p[pi].off, v, (size_t)n * sizeof(float));
  s->gset |= 1u << pi;
  return true;
}
bool cw_surface_set_global_texture(const char *name, const char *param, t3_texture *t) {
  surface *s;
  int pi;
  if (!locate("cw_surface_set_global_texture", NULL, name, param, &s, &pi, NULL) || !texture_param("cw_surface_set_global_texture", s, pi)) return false;
  t3_texture **slot = &s->gtex[s->p[pi].off];
  if (t) t3_retain(t);
  if (*slot) t3_release(*slot);
  *slot = t;
  return true;
}

/* ── the node type ────────────────────────────────────────────────────────
 * A surface as a node: its properties are the parameters (a float, vector or
 * colour each; a texture is a resource). Under a Node3D, the meshes of that
 * node (its own object's subtree, not its child nodes') draw with clones of
 * their materials that carry the surface and the node's values. Shared
 * materials stay shared inside the node: one clone per source material. */
typedef struct snode_pair { t3_material *src, *clone; } snode_pair; /* both retained */
typedef struct snode {
  surface *s;
  CW_ARRAY(snode_pair) pairs;
  uint64_t *tex_uid; /* per texture parameter: the resource the clone textures came from */
  cw_res *tex_res;
  /* then one 16-byte slot per parameter: floats, or a texture's resource uid */
} snode;
#define SLOT(d, i) ((uint8_t *)(d) + sizeof(snode) + (size_t)(i) * 16)

static surface *surface_of_type(const cw_node_type *t) {
  for (; t; t = t->base)
    for (int i = 0; i < nS; i++)
      if (S[i]->node_type == t) return S[i];
  return NULL;
}

static void snode_init(cw_node n, void *data) {
  snode *d = data;
  d->s = surface_of_type(cw_node_get_type(n));
  if (!d->s) return;
  for (int i = 0; i < d->s->np; i++) {
    const sparam *p = &d->s->p[i];
    if (p->type != CW_SURFACE_TEXTURE && p->n <= 4) memcpy(SLOT(d, i), d->s->defaults + p->off, (size_t)p->n * sizeof(float));
  }
  d->tex_uid = cw_calloc((size_t)CW_MAX(d->s->ntex, 1), sizeof *d->tex_uid, CW_MEM_RENDER);
  d->tex_res = cw_calloc((size_t)CW_MAX(d->s->ntex, 1), sizeof *d->tex_res, CW_MEM_RENDER);
}

typedef struct { snode *d; t3_object *root; } swalk;
/* the node's own meshes: its object's subtree, stopping at another node's object is not possible to tell here, so
 * every mesh below takes it (a child node with a surface node of its own replaces it again after) */
static void snode_visit(t3_object *o, void *u) {
  swalk *w = u;
  snode *d = w->d;
  if (o->type != T3_MESH && o->type != T3_SKINNED_MESH && o->type != T3_INSTANCED_MESH) return;
  t3_mesh *m = (t3_mesh *)o;
  for (int k = 0; k < m->material_count; k++) {
    t3_material *mt = m->materials[k];
    if (!mt || cw_shading_has(mt, "meshdecal")) continue; /* (a decal clipped onto the mesh keeps its own look) */
    t3_material *to = NULL;
    for (int j = 0; j < d->pairs.count && !to; j++) {
      if (d->pairs.data[j].clone == mt) to = mt;
      else if (d->pairs.data[j].src == mt) to = d->pairs.data[j].clone;
    }
    if (!to) {
      to = t3_material_clone(mt);
      if (!cw_surface_apply(to, d->s->name)) {
        t3_release(to);
        continue;
      }
      cw_array_push(&d->pairs, ((snode_pair){ t3_retain(mt), to }));
    }
    if (to != mt) {
      t3_retain(to);
      t3_release(mt);
      m->materials[k] = to;
    }
  }
}

static void snode_restore_visit(t3_object *o, void *u) {
  snode *d = ((swalk *)u)->d;
  if (o->type != T3_MESH && o->type != T3_SKINNED_MESH && o->type != T3_INSTANCED_MESH) return;
  t3_mesh *m = (t3_mesh *)o;
  for (int k = 0; k < m->material_count; k++)
    for (int j = 0; j < d->pairs.count; j++)
      if (d->pairs.data[j].clone == m->materials[k]) {
        t3_retain(d->pairs.data[j].src);
        t3_release(m->materials[k]);
        m->materials[k] = d->pairs.data[j].src;
        break;
      }
}

static void snode_release(cw_node n, snode *d) {
  if (!d->s) return;
  t3_object *o = cw_node_valid(n) ? cw_node3d_object(cw_node_parent(n)) : NULL;
  swalk w = { d, o };
  if (o) t3_object_traverse(o, snode_restore_visit, &w);
  for (int j = 0; j < d->pairs.count; j++) {
    cw_surface_remove(d->pairs.data[j].clone, d->s->name);
    t3_release(d->pairs.data[j].clone);
    t3_release(d->pairs.data[j].src);
  }
  cw_array_free(&d->pairs);
}

static void snode_exit(cw_node n, void *data) { snode_release(n, data); }
static void snode_destroy(cw_node n, void *data) {
  snode *d = data;
  snode_release(n, d);
  for (int i = 0; d->s && i < d->s->ntex; i++) cw_res_release(d->tex_res[i]);
  cw_free(d->tex_uid);
  cw_free(d->tex_res);
}

static void snode_pre_render(cw_node n, void *data, double alpha) {
  (void)alpha;
  snode *d = data;
  if (!d->s) return;
  t3_object *o = cw_node3d_object(cw_node_parent(n));
  if (!o) return;
  swalk w = { d, o };
  t3_object_traverse(o, snode_visit, &w);
  for (int i = 0; i < d->s->np; i++) {
    const sparam *p = &d->s->p[i];
    if (p->type == CW_SURFACE_TEXTURE) {
      uint64_t uid;
      memcpy(&uid, SLOT(d, i), sizeof uid);
      if (uid != d->tex_uid[p->off]) {
        cw_res_release(d->tex_res[p->off]);
        d->tex_res[p->off] = uid ? cw_res_load_uid(uid, CW_RES_TEXTURE) : (cw_res){ 0 };
        d->tex_uid[p->off] = uid;
      }
      t3_texture *t = uid ? cw_res_texture(d->tex_res[p->off]) : NULL; /* (NULL while it loads: the surface's value) */
      for (int j = 0; j < d->pairs.count; j++) cw_surface_set_texture(d->pairs.data[j].clone, d->s->name, p->name, t);
    } else if (p->n <= 4) {
      for (int j = 0; j < d->pairs.count; j++) cw_surface_set(d->pairs.data[j].clone, d->s->name, p->name, (const float *)(void *)SLOT(d, i), p->n);
    }
  }
}

static void node_type_make(surface *s, const char *type_name) {
  static const cw_type types[] = { CW_T_FLOAT, CW_T_VEC2, CW_T_VEC3, CW_T_VEC4, CW_T_COLOR, CW_T_NONE, CW_T_RES };
  cw_prop *props = cw_calloc((size_t)s->np + 1, sizeof *props, CW_MEM_RENDER);
  int k = 0;
  for (int i = 0; i < s->np; i++) {
    const sparam *p = &s->p[i];
    if (p->type == CW_SURFACE_MAT4 || p->n > 4) continue; /* a matrix or an array is set from code */
    props[k++] = (cw_prop){ .name = cw_intern(p->name), .type = types[p->type], .flags = CW_PROP_DEFAULT,
                            .offset = (int)(sizeof(snode) + (size_t)i * 16),
                            .size = p->type == CW_SURFACE_TEXTURE ? (int)sizeof(uint64_t) : p->n * (int)sizeof(float),
                            .min = -1e9, .max = 1e9,
                            .doc = p->type == CW_SURFACE_TEXTURE ? "Surface parameter (a texture)." : "Surface parameter." };
  }
  cw_scene_register_builtin_types(); /* (a game registers its surfaces in cw_main, before the runner has: "Node" must exist) */
  cw_node_type *t = cw_calloc(1, sizeof *t, CW_MEM_RENDER);
  char doc[160];
  snprintf(doc, sizeof doc, "The game's '%s' surface on its parent's meshes (cartwheel/surface.h); the properties are its parameters.", s->name);
  *t = (cw_node_type){ .name = cw_intern(type_name), .parent = "Node", .data_size = sizeof(snode) + (size_t)s->np * 16, .props = props,
                       .doc = cw_intern(doc), .init = snode_init, .destroy = snode_destroy, .exit_tree = snode_exit,
                       .pre_render = snode_pre_render };
  s->node_type = t;
  s->node_props = props;
  cw_node_type_register(t);
}

/* a fresh engine (tests): forget every material; the surfaces stay registered */
void cw__surface_shutdown(void);
void cw__surface_shutdown(void) {
  for (int i = 0; i < nS; i++) {
    surface *s = S[i];
    for (int j = 0; j < s->recs.count; j++) rec_free(s, &s->recs.data[j]);
    cw_array_free(&s->recs);
    cw_map_free(&s->index_of);
    for (int j = 0; j < s->ntex; j++)
      if (s->gtex[j]) t3_release(s->gtex[j]), s->gtex[j] = NULL;
  }
  if (white) t3_release(white), white = NULL;
}

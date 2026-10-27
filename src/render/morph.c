/* Morph target weights by name (cartwheel/morph.h). */
#include <string.h>

#include "cartwheel/morph.h"

static bool morphed(const t3_object *o) {
  return (o->type == T3_MESH || o->type == T3_SKINNED_MESH) && ((const t3_mesh *)o)->morph_influence_count > 0;
}

typedef struct { const char *name; float w; int n; bool get; float out; } set_ctx;
static void set_rec(t3_object *o, set_ctx *c) {
  if (morphed(o)) {
    t3_mesh *m = (t3_mesh *)o;
    int i = t3_mesh_get_morph_target_index(m, c->name);
    if (i >= 0) {
      if (c->get) {
        if (!c->n) c->out = m->morph_influences[i];
      } else {
        m->morph_influences[i] = c->w;
      }
      c->n++;
      if (c->get) return;
    }
  }
  for (int k = 0; k < o->child_count; k++) {
    if (c->get && c->n) return;
    set_rec(o->children[k], c);
  }
}

int cw_morph_set(t3_object *root, const char *name, float weight) {
  if (!root || !name) return 0;
  set_ctx c = { name, weight, 0, false, 0 };
  set_rec(root, &c);
  return c.n;
}

float cw_morph_get(const t3_object *root, const char *name) {
  if (!root || !name) return 0;
  set_ctx c = { name, 0, 0, true, 0 };
  set_rec((t3_object *)root, &c);
  return c.out;
}

static int reset_rec(t3_object *o) {
  int n = 0;
  if (morphed(o)) {
    t3_mesh *m = (t3_mesh *)o;
    memset(m->morph_influences, 0, sizeof(float) * (size_t)m->morph_influence_count);
    n++;
  }
  for (int k = 0; k < o->child_count; k++) n += reset_rec(o->children[k]);
  return n;
}
int cw_morph_reset(t3_object *root) { return root ? reset_rec(root) : 0; }

typedef struct { const char *seen[256]; int n; } names_ctx;
static void names_rec(const t3_object *o, names_ctx *c) {
  if (morphed(o)) {
    const t3_mesh *m = (const t3_mesh *)o;
    for (int i = 0; m->morph_target_names && i < m->morph_influence_count; i++) {
      const char *nm = m->morph_target_names[i];
      if (!nm || !nm[0]) continue;
      bool dup = false;
      for (int j = 0; j < c->n && !dup; j++) dup = !strcmp(c->seen[j], nm);
      if (dup || c->n == 256) continue;
      c->seen[c->n++] = nm;
    }
  }
  for (int k = 0; k < o->child_count; k++) names_rec(o->children[k], c);
}
int cw_morph_names(const t3_object *root, const char **out, int cap) {
  if (!root) return 0;
  names_ctx c;
  c.n = 0;
  names_rec(root, &c);
  for (int i = 0; out && i < c.n && i < cap; i++) out[i] = c.seen[i];
  return c.n;
}

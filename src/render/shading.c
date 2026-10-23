/* Shading parts (cartwheel/shading.h): several three.c generator extensions
 * on one material, by a composed extension name and a hook that calls each
 * part's hook in order. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cartwheel/shading.h"

enum { NAME_MAX_ = 32, PART_MAX_ = 16 };

/* the fixed order: surface, lighting, (others by name), indirect */
static int rank_of(const char *p) {
  static const char *const surface[] = { "terrain", "terrainlow", "terrainfar", "water" };
  for (size_t i = 0; i < sizeof surface / sizeof surface[0]; i++)
    if (!strcmp(p, surface[i])) return 0;
  if (!strcmp(p, "cluster")) return 1;
  if (!strcmp(p, "gi")) return 3;
  return 2;
}
static int part_cmp(const void *a, const void *b) {
  const char *x = a, *y = b;
  int rx = rank_of(x), ry = rank_of(y);
  return rx != ry ? rx - ry : strcmp(x, y);
}

/* split an extension name into parts; returns the count */
static int split(const char *ext, char parts[CW_SHADING_MAX_PARTS + 1][PART_MAX_]) {
  int n = 0;
  if (!ext || !ext[0]) return 0;
  const char *p = ext;
  while (*p && n <= CW_SHADING_MAX_PARTS) {
    const char *e = strchr(p, '_');
    size_t len = e ? (size_t)(e - p) : strlen(p);
    if (len && len < PART_MAX_) {
      memcpy(parts[n], p, len);
      parts[n][len] = 0;
      n++;
    }
    if (!e) break;
    p = e + 1;
  }
  return n;
}

/* ── the run-time hooks ───────────────────────────────────────────────── */

typedef struct part_reg {
  t3_renderer *r;
  char name[PART_MAX_];
  t3_extension_fill fill;
  void *user;
  bool batching; /* cw_shading_part_batching: the hook reads nothing of the object */
} part_reg;

typedef struct composite {
  t3_renderer *r;
  char name[NAME_MAX_];
  int n;
  t3_extension_fill fill[CW_SHADING_MAX_PARTS];
  void *user[CW_SHADING_MAX_PARTS];
} composite;

static struct {
  part_reg *parts;
  int nparts, cap_parts;
  composite **comps; /* per (renderer, composed name); stable pointers (hook user data) */
  int ncomps, cap_comps;
  char (*names)[NAME_MAX_]; /* every composed name a material carried */
  int nnames, cap_names;
} S;

static const part_reg *find_part(t3_renderer *r, const char *name) {
  for (int i = 0; i < S.nparts; i++)
    if (S.parts[i].r == r && !strcmp(S.parts[i].name, name)) return &S.parts[i];
  return NULL;
}

static void composite_fill(t3_extension_ctx *c, void *user) {
  composite *k = user;
  for (int i = 0; i < k->n; i++)
    if (k->fill[i]) k->fill[i](c, k->user[i]);
}

/* (re)install composed name `name` on r: resolved from r's current parts */
static void install(t3_renderer *r, const char *name) {
  char parts[CW_SHADING_MAX_PARTS + 1][PART_MAX_];
  int n = split(name, parts);
  if (n < 2) return; /* a single part is its own hook */
  composite *k = NULL;
  for (int i = 0; i < S.ncomps; i++)
    if (S.comps[i]->r == r && !strcmp(S.comps[i]->name, name)) k = S.comps[i];
  if (!k) {
    if (S.ncomps == S.cap_comps) {
      S.cap_comps = S.cap_comps ? S.cap_comps * 2 : 8;
      S.comps = realloc(S.comps, sizeof *S.comps * (size_t)S.cap_comps);
    }
    k = calloc(1, sizeof *k);
    k->r = r;
    snprintf(k->name, sizeof k->name, "%s", name);
    S.comps[S.ncomps++] = k;
  }
  k->n = n;
  bool any = false, batching = true;
  for (int i = 0; i < n; i++) {
    const part_reg *p = find_part(r, parts[i]);
    k->fill[i] = p ? p->fill : NULL;
    k->user[i] = p ? p->user : NULL;
    any |= p != NULL && p->fill;
    batching &= p != NULL && p->batching;
  }
  /* a composed draw batches when every part's hook allows it */
  t3_renderer_set_extension_batching(r, name, batching);
  /* a composed draw with a part nobody fills on this renderer still draws:
   * that part's values keep their last (or zero) values */
  t3_renderer_set_extension(r, name, any ? composite_fill : NULL, any ? k : NULL);
}

static bool part_name_ok(const char *fn, const char *name) {
  if (name && name[0] && strlen(name) < PART_MAX_ && !strchr(name, '_')) return true;
  cw_warn("%s: '%s' is not a part name ([a-z0-9], under %d characters)", fn, name ? name : "(null)", PART_MAX_);
  return false;
}

static part_reg *add_part(t3_renderer *r, const char *name) {
  if (S.nparts == S.cap_parts) {
    S.cap_parts = S.cap_parts ? S.cap_parts * 2 : 8;
    S.parts = realloc(S.parts, sizeof *S.parts * (size_t)S.cap_parts);
  }
  part_reg *p = &S.parts[S.nparts++];
  memset(p, 0, sizeof *p);
  p->r = r;
  snprintf(p->name, sizeof p->name, "%s", name);
  return p;
}

/* re-install every composed name that includes part `name` */
static void reinstall(t3_renderer *r, const char *name) {
  for (int i = 0; i < S.nnames; i++) {
    char parts[CW_SHADING_MAX_PARTS + 1][PART_MAX_];
    int n = split(S.names[i], parts);
    for (int j = 0; j < n; j++)
      if (!strcmp(parts[j], name)) {
        install(r, S.names[i]);
        break;
      }
  }
}

void cw_shading_part_batching(t3_renderer *r, const char *name, bool on) {
  if (!r || !part_name_ok("cw_shading_part_batching", name)) return;
  part_reg *p = (part_reg *)find_part(r, name);
  if (!p) p = add_part(r, name);
  p->batching = on;
  t3_renderer_set_extension_batching(r, name, on);
  reinstall(r, name);
}

void cw_shading_part(t3_renderer *r, const char *name, t3_extension_fill fill, void *user) {
  if (!r || !part_name_ok("cw_shading_part", name)) return;
  part_reg *p = (part_reg *)find_part(r, name);
  if (!p && fill) {
    if (S.nparts == S.cap_parts) {
      S.cap_parts = S.cap_parts ? S.cap_parts * 2 : 8;
      S.parts = realloc(S.parts, sizeof *S.parts * (size_t)S.cap_parts);
    }
    p = add_part(r, name);
  }
  if (p) {
    p->fill = fill;
    p->user = user;
  }
  t3_renderer_set_extension(r, name, fill, user);
  reinstall(r, name);
}

bool cw_shading_part_known(const char *name) {
  if (!name || !name[0]) return false;
  for (int i = 0; i < S.nparts; i++)
    if (!strcmp(S.parts[i].name, name)) return true;
  return false;
}

void cw_shading_forget(t3_renderer *r) {
  int w = 0;
  for (int i = 0; i < S.nparts; i++)
    if (S.parts[i].r != r) S.parts[w++] = S.parts[i];
  S.nparts = w;
  w = 0;
  for (int i = 0; i < S.ncomps; i++) {
    if (S.comps[i]->r == r) free(S.comps[i]);
    else S.comps[w++] = S.comps[i];
  }
  S.ncomps = w;
}

/* a composed name seen for the first time: install it on every renderer
 * with a part of it */
static void note_name(const char *name) {
  if (!strchr(name, '_')) return;
  for (int i = 0; i < S.nnames; i++)
    if (!strcmp(S.names[i], name)) return;
  if (S.nnames == S.cap_names) {
    S.cap_names = S.cap_names ? S.cap_names * 2 : 8;
    S.names = realloc(S.names, sizeof *S.names * (size_t)S.cap_names);
  }
  snprintf(S.names[S.nnames++], NAME_MAX_, "%s", name);
  t3_renderer *seen[64];
  int ns = 0;
  for (int i = 0; i < S.nparts; i++) {
    bool dup = false;
    for (int j = 0; j < ns; j++) dup |= seen[j] == S.parts[i].r;
    if (dup) continue;
    if (ns < 64) seen[ns++] = S.parts[i].r;
    install(S.parts[i].r, name);
  }
}

/* ── materials ────────────────────────────────────────────────────────── */

bool cw_shading_set(t3_material *m, const char *const *names, int n) {
  if (!m) return false;
  char parts[CW_SHADING_MAX_PARTS + 1][PART_MAX_];
  int k = 0;
  for (int i = 0; i < n; i++) {
    if (!names[i] || !names[i][0] || strlen(names[i]) >= PART_MAX_) return false;
    bool dup = false;
    for (int j = 0; j < k; j++) dup |= !strcmp(parts[j], names[i]);
    if (dup) continue;
    if (k == CW_SHADING_MAX_PARTS) return false;
    snprintf(parts[k++], PART_MAX_, "%s", names[i]);
  }
  qsort(parts, (size_t)k, PART_MAX_, part_cmp);
  char name[NAME_MAX_ + PART_MAX_] = "";
  for (int i = 0; i < k; i++) {
    if (i) strcat(name, "_");
    strcat(name, parts[i]);
  }
  if (strlen(name) >= NAME_MAX_) return false;
  const char *cur = t3_material_get_extension(m);
  if (cur ? !strcmp(cur, name) : !name[0]) return true;
  if (!t3_material_set_extension(m, k ? name : NULL)) return false;
  if (k > 1) note_name(name);
  return true;
}

static int parts_of(const t3_material *m, const char *out[CW_SHADING_MAX_PARTS + 1], char store[CW_SHADING_MAX_PARTS + 1][PART_MAX_]) {
  int n = split(m ? t3_material_get_extension(m) : NULL, store);
  for (int i = 0; i < n; i++) out[i] = store[i];
  return n;
}

bool cw_shading_has(const t3_material *m, const char *name) {
  char store[CW_SHADING_MAX_PARTS + 1][PART_MAX_];
  const char *p[CW_SHADING_MAX_PARTS + 1];
  int n = parts_of(m, p, store);
  for (int i = 0; i < n; i++)
    if (!strcmp(p[i], name)) return true;
  return false;
}

bool cw_shading_add(t3_material *m, const char *name) {
  if (!m || cw_shading_has(m, name)) return m != NULL;
  char store[CW_SHADING_MAX_PARTS + 1][PART_MAX_];
  const char *p[CW_SHADING_MAX_PARTS + 2];
  int n = parts_of(m, p, store);
  if (n >= CW_SHADING_MAX_PARTS) return false;
  p[n++] = name;
  return cw_shading_set(m, p, n);
}

void cw_shading_remove(t3_material *m, const char *name) {
  char store[CW_SHADING_MAX_PARTS + 1][PART_MAX_];
  const char *p[CW_SHADING_MAX_PARTS + 1];
  int n = parts_of(m, p, store), k = 0;
  for (int i = 0; i < n; i++)
    if (strcmp(p[i], name)) p[k++] = p[i];
  if (k != n) cw_shading_set(m, p, k);
}

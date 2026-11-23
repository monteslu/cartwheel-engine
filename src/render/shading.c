/* Shading parts (cartwheel/shading.h): several three.c generator extensions
 * on one material, by a composed extension name and a hook that calls each
 * part's hook in order. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cartwheel/shading.h"

enum { NAME_MAX_ = 64, PART_MAX_ = 16 };

/* the fixed order: surface, lighting, (others by name), indirect */
static int rank_of(const char *p) {
  static const char *const surface[] = { "terrain", "terrainlow", "terrainmin", "terrainfx", "terrainfxlow", "terrainfxmin", "terrainfar", "water", "waterlow" };
  for (size_t i = 0; i < sizeof surface / sizeof surface[0]; i++)
    if (!strcmp(p, surface[i])) return 0;
  if (!strcmp(p, "cluster") || !strcmp(p, "clusteries")) return 1;
  if (!strcmp(p, "gi") || !strcmp(p, "gibuf")) return 3;
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

static void note_name_fwd(const char *name);
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

/* ── traits and parts for every renderer ──────────────────────────────── */
typedef struct global_part { char name[PART_MAX_]; unsigned traits; t3_extension_fill fill; void *user; bool batching, everywhere; } global_part;
static global_part *G;
static int nG, capG;
static t3_renderer **R; /* every renderer cw__shading_renderer_init saw */
static int nR, capR;

static global_part *global_of(const char *name, bool make) {
  for (int i = 0; i < nG; i++)
    if (!strcmp(G[i].name, name)) return &G[i];
  if (!make) return NULL;
  if (nG == capG) {
    capG = capG ? capG * 2 : 8;
    G = realloc(G, sizeof *G * (size_t)capG);
  }
  global_part *g = &G[nG++];
  memset(g, 0, sizeof *g);
  snprintf(g->name, sizeof g->name, "%s", name);
  return g;
}

/* CW_TEST_SURFACE_ALONE=1 (native test runs: the control of tests/custom-surface.mjs): a game's surface part
 * is treated as any part was before traits existed (no probes, no decals, dropped from the G-buffer), and a
 * material takes four parts at most */
static bool surfaces_alone(void) {
  static int v = -1;
  if (v < 0) {
    const char *e = getenv("CW_TEST_SURFACE_ALONE");
    v = e && atoi(e);
  }
  return v;
}
static int max_parts(void) { return surfaces_alone() ? 4 : CW_SHADING_MAX_PARTS; }

void cw_shading_part_traits(const char *name, unsigned traits) {
  if (!part_name_ok("cw_shading_part_traits", name)) return;
  global_of(name, true)->traits = traits;
}
unsigned cw_shading_part_traits_of(const char *name) {
  const global_part *g = name && !surfaces_alone() ? global_of(name, false) : NULL;
  return g ? g->traits : 0;
}

void cw_shading_part_everywhere(const char *name, t3_extension_fill fill, void *user, bool batching) {
  if (!part_name_ok("cw_shading_part_everywhere", name)) return;
  global_part *g = global_of(name, true);
  g->fill = fill, g->user = user, g->batching = batching, g->everywhere = fill != NULL;
  for (int i = 0; i < nR; i++) {
    cw_shading_part(R[i], name, fill, user);
    cw_shading_part_batching(R[i], name, batching);
  }
}

/* a renderer was made (src/scene/render3d.c): it takes the parts registered for every renderer */
void cw__shading_renderer_init(t3_renderer *r);
void cw__shading_renderer_init(t3_renderer *r) {
  if (!r) return;
  for (int i = 0; i < nR; i++)
    if (R[i] == r) return;
  if (nR == capR) {
    capR = capR ? capR * 2 : 4;
    R = realloc(R, sizeof *R * (size_t)capR);
  }
  R[nR++] = r;
  for (int i = 0; i < nG; i++)
    if (G[i].everywhere) {
      cw_shading_part(r, G[i].name, G[i].fill, G[i].user);
      cw_shading_part_batching(r, G[i].name, G[i].batching);
    }
}

/* the G-buffer program of a material whose extension is `ext` (src/gi/ssr.c): its surface parts, then gibuf
 * ("dissolve_gibuf"; "gibuf" alone for a material with none) */
void cw__shading_gbuf_name(const char *ext, char out[64]);
void cw__shading_gbuf_name(const char *ext, char out[64]) {
  char parts[CW_SHADING_MAX_PARTS + 1][PART_MAX_];
  int n = split(ext, parts);
  out[0] = 0;
  for (int i = 0; i < n; i++)
    if (cw_shading_part_traits_of(parts[i]) & CW_PART_SURFACE) strcat(out, parts[i]), strcat(out, "_");
  strcat(out, "gibuf");
  if (strlen(out) > 5) note_name_fwd(out);
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
  w = 0;
  for (int i = 0; i < nR; i++)
    if (R[i] != r) R[w++] = R[i];
  nR = w;
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
  /* (a name is made of the parts of some renderer: every renderer there is, and the parts' own) */
  for (int i = 0; i < nR; i++) install(R[i], name);
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

static void note_name_fwd(const char *name) { note_name(name); }

/* a material that cannot take a part says so once per composed name: it keeps the parts it had, so a
 * light, a probe or the game's own surface is missing from it */
static void refuse(const t3_material *m, char parts[][PART_MAX_], int k, const char *why) {
  static char said[16][NAME_MAX_ * 2];
  static int nsaid;
  char list[NAME_MAX_ * 2] = "";
  for (int i = 0; i < k && strlen(list) + PART_MAX_ + 2 < sizeof list; i++) strcat(list, i ? " " : ""), strcat(list, parts[i]);
  for (int i = 0; i < nsaid; i++)
    if (!strcmp(said[i], list)) return;
  if (nsaid < 16) snprintf(said[nsaid++], sizeof said[0], "%s", list);
  cw_error("shading: material%s%s%s cannot carry the parts [%s]: %s. It keeps the parts it had (x%s), so part of its "
           "shading is missing (cartwheel/shading.h)",
           m->name && *m->name ? " \"" : "", m->name && *m->name ? m->name : "", m->name && *m->name ? "\"" : "", list, why,
           t3_material_get_extension(m) ? t3_material_get_extension(m) : "");
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
    if (k == max_parts()) {
      snprintf(parts[k], PART_MAX_, "%s", names[i]);
      refuse(m, parts, k + 1, "a material takes at most CW_SHADING_MAX_PARTS parts");
      return false;
    }
    snprintf(parts[k++], PART_MAX_, "%s", names[i]);
  }
  qsort(parts, (size_t)k, PART_MAX_, part_cmp);
  char name[(CW_SHADING_MAX_PARTS + 1) * PART_MAX_] = "";
  for (int i = 0; i < k; i++) {
    if (i) strcat(name, "_");
    strcat(name, parts[i]);
  }
  if (strlen(name) >= NAME_MAX_) {
    refuse(m, parts, k, "the composed name passes 63 characters (shorter part names)");
    return false;
  }
  const char *cur = t3_material_get_extension(m);
  if (cur ? !strcmp(cur, name) : !name[0]) return true;
  if (!t3_material_set_extension(m, k ? name : NULL)) {
    refuse(m, parts, k, "three.c refused the name");
    return false;
  }
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

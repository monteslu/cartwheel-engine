/* cwcsg: bake a scene's CSG shapes into meshes (cartwheel/csg.h).
 *
 *   cwcsg <scene.cwscene> [--assets DIR] [--check] [--watch] [-q]
 *
 * Loads the scene into a tree (no window, no GPU), and for every CSG root
 * with a bake_file runs its shapes through Manifold's booleans and writes
 * DIR/<bake_file> (DIR: the asset root, default the scene's directory).
 * Prints one JSON line per root. --check writes nothing and exits 1 when a
 * file is missing or differs from what the shapes give; --watch bakes again
 * whenever the scene file changes (Ctrl+C stops it).
 *
 * The output is canonical, so the same scene gives the same bytes on every
 * run: positions snapped to 0.1 mm, flat-shaded faces, triangles rotated to
 * start at their smallest corner and sorted, vertices sorted. Manifold's own
 * triangle and vertex order never reaches the file. */
#include <errno.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>

#include "cartwheel/csg.h"
#include "cartwheel/jobs.h"
#include "cartwheel/platform.h"
#include "cartwheel/scene_file.h"
#include "manifold/manifoldc.h"

#define QUANT 10000.0  /* positions: 0.1 mm steps */
#define NQUANT 4096.0  /* normals */

static bool quiet;

static double now_ms(void) {
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return ts.tv_sec * 1e3 + ts.tv_nsec / 1e6;
}

static char *read_file(const char *path, size_t *len) {
  FILE *f = fopen(path, "rb");
  if (!f) return NULL;
  fseek(f, 0, SEEK_END);
  long n = ftell(f);
  fseek(f, 0, SEEK_SET);
  char *b = malloc((size_t)n + 1);
  size_t got = fread(b, 1, (size_t)n, f);
  fclose(f);
  b[got] = 0;
  if (len) *len = got;
  return b;
}

static void mkdirs_for(const char *path) {
  char tmp[8400];
  snprintf(tmp, sizeof tmp, "%s", path);
  for (char *p = tmp + 1; *p; p++)
    if (*p == '/') {
      *p = 0;
      mkdir(tmp, 0755);
      *p = '/';
    }
}

/* ── Manifold ───────────────────────────────────────────────────────── */

typedef struct bake {
  const cw_csg_shape *s;
  int n;
  int *orig;    /* Manifold original id per shape (-1: none) */
} bake;

static ManifoldManifold *transformed(ManifoldManifold *m, const t3_mat4 *t) {
  const float *e = t->e;
  ManifoldManifold *r = manifold_transform(manifold_alloc_manifold(), m, e[0], e[1], e[2], e[4], e[5], e[6], e[8], e[9],
                                           e[10], e[12], e[13], e[14]);
  manifold_delete_manifold(m);
  return r;
}

static ManifoldManifold *primitive(bake *b, int i) {
  const cw_csg_shape *s = &b->s[i];
  ManifoldManifold *m = NULL;
  t3_mat4 t = s->to_root;
  switch (s->kind) {
  case CW_CSG_BOX: m = manifold_cube(manifold_alloc_manifold(), s->size.x, s->size.y, s->size.z, 1); break;
  case CW_CSG_CYLINDER: {
    /* Manifold's cylinder runs along Z; ours along Y: rotate -90 degrees about X (Z -> Y) first */
    m = manifold_cylinder(manifold_alloc_manifold(), s->height, s->radius, s->cone ? 0 : s->radius, CW_MAX(s->segments, 3), 1);
    t3_mat4 r, out;
    t3_mat4_identity(&r);
    r.e[5] = 0, r.e[6] = -1, r.e[9] = 1, r.e[10] = 0; /* columns: x -> x, y -> -z, z -> y */
    t3_mat4_multiply(&out, &s->to_root, &r);
    t = out;
    break;
  }
  case CW_CSG_SPHERE: m = manifold_sphere(manifold_alloc_manifold(), s->radius, CW_MAX(s->segments, 4)); break;
  default: return manifold_empty(manifold_alloc_manifold());
  }
  m = transformed(m, &t);
  ManifoldManifold *o = manifold_as_original(manifold_alloc_manifold(), m);
  manifold_delete_manifold(m);
  b->orig[i] = manifold_original_id(o);
  return o;
}

static ManifoldManifold *eval(bake *b, int i) {
  ManifoldManifold *acc = primitive(b, i);
  for (int j = i + 1; j < b->n; j++) {
    if (b->s[j].parent != i) continue;
    ManifoldManifold *c = eval(b, j);
    ManifoldOpType op = b->s[j].op == CW_CSG_SUBTRACTION ? MANIFOLD_SUBTRACT
                        : b->s[j].op == CW_CSG_INTERSECTION ? MANIFOLD_INTERSECT : MANIFOLD_ADD;
    ManifoldManifold *r = manifold_boolean(manifold_alloc_manifold(), acc, c, op);
    manifold_delete_manifold(acc);
    manifold_delete_manifold(c);
    acc = r;
  }
  return acc;
}

/* ── canonical output ───────────────────────────────────────────────── */

typedef struct vkey { int64_t p[3]; int32_t n[3]; int32_t color; } vkey; /* compared field by field */
typedef struct tri { vkey v[3]; } tri;

static int cmp_vkey(const vkey *a, const vkey *b) {
  for (int i = 0; i < 3; i++)
    if (a->p[i] != b->p[i]) return a->p[i] < b->p[i] ? -1 : 1;
  for (int i = 0; i < 3; i++)
    if (a->n[i] != b->n[i]) return a->n[i] < b->n[i] ? -1 : 1;
  return a->color < b->color ? -1 : a->color > b->color;
}
static int cmp_vkey_q(const void *a, const void *b) { return cmp_vkey(a, b); }
static int cmp_tri(const void *a, const void *b) {
  const tri *x = a, *y = b;
  for (int i = 0; i < 3; i++) {
    int c = cmp_vkey(&x->v[i], &y->v[i]);
    if (c) return c;
  }
  return 0;
}

typedef struct baked {
  uint8_t *bytes;
  size_t len;
  int vertices, triangles;
} baked;

static bool bake_root(cw_node root, baked *out) {
  int n = cw_csg_flatten(root, NULL, 0);
  cw_csg_shape *s = calloc((size_t)n, sizeof *s);
  cw_csg_flatten(root, s, n);
  bake b = { s, n, calloc((size_t)n, sizeof(int)) };
  for (int i = 0; i < n; i++) b.orig[i] = -1;
  ManifoldManifold *m = eval(&b, 0);
  ManifoldError err = manifold_status(m);
  if (err != MANIFOLD_NO_ERROR) {
    fprintf(stderr, "cwcsg: '%s': Manifold failed (error %d)\n", cw_node_name(root), (int)err);
    return false;
  }
  ManifoldMeshGL *g = manifold_get_meshgl(manifold_alloc_meshgl(), m);
  size_t np = manifold_meshgl_num_prop(g), nt = manifold_meshgl_num_tri(g);
  float *props = malloc(manifold_meshgl_vert_properties_length(g) * sizeof(float) + 4);
  uint32_t *tv = malloc(nt * 3 * sizeof(uint32_t) + 4);
  size_t nrun = manifold_meshgl_run_original_id_length(g);
  uint32_t *run_idx = malloc((manifold_meshgl_run_index_length(g) + 1) * sizeof(uint32_t)), *run_orig = malloc(nrun * sizeof(uint32_t) + 4);
  manifold_meshgl_vert_properties(props, g);
  manifold_meshgl_tri_verts(tv, g);
  manifold_meshgl_run_index(run_idx, g);
  manifold_meshgl_run_original_id(run_orig, g);

  tri *tris = malloc(nt * sizeof *tris + 1);
  int kept = 0;
  size_t run = 0;
  for (size_t t = 0; t < nt; t++) {
    while (run + 1 < nrun && 3 * t >= run_idx[run + 1]) run++;
    int shape = 0;
    for (int i = 0; i < n; i++)
      if (b.orig[i] >= 0 && (uint32_t)b.orig[i] == run_orig[run]) shape = i;
    int64_t q[3][3];
    for (int c = 0; c < 3; c++)
      for (int k = 0; k < 3; k++) q[c][k] = llround(props[(size_t)tv[3 * t + c] * np + (size_t)k] * QUANT);
    double e1[3], e2[3], nn[3];
    for (int k = 0; k < 3; k++) e1[k] = (double)(q[1][k] - q[0][k]), e2[k] = (double)(q[2][k] - q[0][k]);
    nn[0] = e1[1] * e2[2] - e1[2] * e2[1];
    nn[1] = e1[2] * e2[0] - e1[0] * e2[2];
    nn[2] = e1[0] * e2[1] - e1[1] * e2[0];
    double len = sqrt(nn[0] * nn[0] + nn[1] * nn[1] + nn[2] * nn[2]);
    if (len < 1e-3) continue; /* collapsed by the snap: twice the area under 1e-11 m2 */
    tri *o = &tris[kept++];
    for (int c = 0; c < 3; c++) {
      memcpy(o->v[c].p, q[c], sizeof q[c]);
      for (int k = 0; k < 3; k++) o->v[c].n[k] = (int32_t)llround(nn[k] / len * NQUANT);
      o->v[c].color = shape;
    }
    /* start at the smallest corner (keeps the winding) */
    int lo = cmp_vkey(&o->v[1], &o->v[0]) < 0 ? 1 : 0;
    if (cmp_vkey(&o->v[2], &o->v[lo]) < 0) lo = 2;
    if (lo) {
      tri r;
      for (int c = 0; c < 3; c++) r.v[c] = o->v[(c + lo) % 3];
      *o = r;
    }
  }
  qsort(tris, (size_t)kept, sizeof *tris, cmp_tri);
  /* unique vertices in sorted order */
  vkey *keys = malloc((size_t)kept * 3 * sizeof *keys + 1);
  for (int t = 0; t < kept; t++)
    for (int c = 0; c < 3; c++) keys[3 * t + c] = tris[t].v[c];
  qsort(keys, (size_t)kept * 3, sizeof *keys, cmp_vkey_q);
  int nv = 0;
  for (int i = 0; i < kept * 3; i++)
    if (!nv || cmp_vkey(&keys[i], &keys[nv - 1])) keys[nv++] = keys[i];
  float *pos = malloc((size_t)nv * 12 + 4), *nor = malloc((size_t)nv * 12 + 4), *col = malloc((size_t)nv * 12 + 4),
        *uv = malloc((size_t)nv * 8 + 4);
  uint32_t *idx = malloc((size_t)kept * 12 + 4);
  cw_csg_mesh cm = { .vertex_count = (uint32_t)nv, .index_count = (uint32_t)kept * 3, .input_hash = cw_csg_input_hash(root) };
  for (int i = 0; i < 3; i++) cm.bounds[i] = 1e30f, cm.bounds[3 + i] = -1e30f;
  for (int i = 0; i < nv; i++) {
    const vkey *k = &keys[i];
    double nx = k->n[0] / NQUANT, ny = k->n[1] / NQUANT, nz = k->n[2] / NQUANT, l = sqrt(nx * nx + ny * ny + nz * nz);
    for (int a = 0; a < 3; a++) {
      pos[3 * i + a] = (float)(k->p[a] / QUANT);
      if (pos[3 * i + a] < cm.bounds[a]) cm.bounds[a] = pos[3 * i + a];
      if (pos[3 * i + a] > cm.bounds[3 + a]) cm.bounds[3 + a] = pos[3 * i + a];
    }
    nor[3 * i] = (float)(nx / l), nor[3 * i + 1] = (float)(ny / l), nor[3 * i + 2] = (float)(nz / l);
    const t3_color *c = &s[k->color].color;
    col[3 * i] = c->r, col[3 * i + 1] = c->g, col[3 * i + 2] = c->b;
    /* box projection: the face's dominant axis picks the plane, one repeat per metre */
    double ax = fabs(nx), ay = fabs(ny), az = fabs(nz);
    float x = pos[3 * i], y = pos[3 * i + 1], z = pos[3 * i + 2];
    if (ax >= ay && ax >= az) uv[2 * i] = nx > 0 ? -z : z, uv[2 * i + 1] = y;
    else if (ay >= az) uv[2 * i] = x, uv[2 * i + 1] = ny > 0 ? -z : z;
    else uv[2 * i] = nz > 0 ? x : -x, uv[2 * i + 1] = y;
  }
  if (!nv) memset(cm.bounds, 0, sizeof cm.bounds);
  for (int t = 0; t < kept; t++)
    for (int c = 0; c < 3; c++) {
      vkey *f = bsearch(&tris[t].v[c], keys, (size_t)nv, sizeof *keys, cmp_vkey_q);
      idx[3 * t + c] = (uint32_t)(f - keys);
    }
  cm.positions = pos, cm.normals = nor, cm.colors = col, cm.uvs = uv, cm.indices = idx;
  out->bytes = cw_csg_mesh_encode(&cm, &out->len);
  out->vertices = nv;
  out->triangles = kept;
  free(pos), free(nor), free(col), free(uv), free(idx), free(keys), free(tris);
  free(props), free(tv), free(run_idx), free(run_orig);
  manifold_delete_meshgl(g);
  manifold_delete_manifold(m);
  free(b.orig);
  free(s);
  return true;
}

/* ── the scene ──────────────────────────────────────────────────────── */

typedef struct roots { cw_node r[256]; int n; } roots;
static bool find_roots(cw_node n, void *u) {
  roots *r = u;
  if (cw_csg_is_root(n) && r->n < 256) r->r[r->n++] = n;
  return true;
}

static int run(const char *scene_path, const char *assets, bool check) {
  char *text = read_file(scene_path, NULL);
  if (!text) {
    fprintf(stderr, "cwcsg: cannot read %s\n", scene_path);
    return 1;
  }
  cw_tree *t = cw_tree_new();
  cw_set_current_tree(t);
  cw_packed_scene *ps = cw_scene_parse(text, scene_path);
  free(text);
  if (!ps) {
    fprintf(stderr, "cwcsg: %s did not parse\n", scene_path);
    cw_tree_free(t);
    return 1;
  }
  /* not added to the tree: nothing enters it (no physics, no loads), the shapes are only read */
  cw_node scene = cw_scene_instance(ps);
  roots rs = { .n = 0 };
  cw_node_walk(scene, find_roots, &rs);
  int bad = 0, baked_n = 0;
  for (int i = 0; i < rs.n; i++) {
    cw_node r = rs.r[i];
    cw_value v;
    const char *file = cw_node_get_prop(r, "bake_file", &v) && v.s ? v.s : "";
    if (!*file) {
      if (!quiet) printf("{\"root\":\"%s\",\"skipped\":\"no bake_file\"}\n", cw_node_name(r));
      continue;
    }
    double t0 = now_ms();
    baked o;
    if (!bake_root(r, &o)) { bad++; continue; }
    double ms = now_ms() - t0;
    char out[8400];
    snprintf(out, sizeof out, "%s/%s", assets, file);
    uint64_t h = cw_csg_fnv1a(o.bytes, o.len, 0xcbf29ce484222325ull);
    const char *state = "written";
    if (check) {
      size_t ol;
      char *old = read_file(out, &ol);
      state = !old ? "missing" : (ol != o.len || memcmp(old, o.bytes, o.len)) ? "differs" : "same";
      if (strcmp(state, "same")) bad++;
      free(old);
    } else {
      mkdirs_for(out);
      FILE *f = fopen(out, "wb");
      if (!f || fwrite(o.bytes, 1, o.len, f) != o.len) {
        fprintf(stderr, "cwcsg: cannot write %s: %s\n", out, strerror(errno));
        bad++;
        state = "failed";
      }
      if (f) fclose(f);
    }
    baked_n++;
    if (!quiet || check)
      printf("{\"root\":\"%s\",\"file\":\"%s\",\"%s\":true,\"shapes\":%d,\"triangles\":%d,\"vertices\":%d,\"bytes\":%zu,"
             "\"hash\":\"%016llx\",\"input_hash\":\"%016llx\",\"ms\":%.2f}\n",
             cw_node_name(r), file, state, cw_csg_flatten(r, NULL, 0), o.triangles, o.vertices, o.len,
             (unsigned long long)h, (unsigned long long)cw_csg_input_hash(r), ms);
    cw_free(o.bytes);
  }
  if (!rs.n) fprintf(stderr, "cwcsg: %s has no CSG shapes\n", scene_path);
  cw_node_free(scene);
  cw_tree_free(t);
  (void)baked_n;
  return bad ? 1 : 0;
}

static int64_t mtime_ns(const char *p) {
  struct stat st;
  if (stat(p, &st)) return -1;
  return (int64_t)st.st_mtim.tv_sec * 1000000000 + st.st_mtim.tv_nsec;
}

int main(int argc, char **argv) {
  const char *scene = NULL, *assets = NULL;
  bool check = false, watch = false;
  for (int i = 1; i < argc; i++) {
    if (!strcmp(argv[i], "--assets") && i + 1 < argc) assets = argv[++i];
    else if (!strcmp(argv[i], "--check")) check = true;
    else if (!strcmp(argv[i], "--watch")) watch = true;
    else if (!strcmp(argv[i], "-q")) quiet = true;
    else if (argv[i][0] != '-' && !scene) scene = argv[i];
    else {
      fprintf(stderr, "usage: cwcsg <scene.cwscene> [--assets DIR] [--check] [--watch] [-q]\n");
      return 2;
    }
  }
  if (!scene) {
    fprintf(stderr, "usage: cwcsg <scene.cwscene> [--assets DIR] [--check] [--watch] [-q]\n");
    return 2;
  }
  char dir[4096];
  if (!assets) {
    snprintf(dir, sizeof dir, "%s", scene);
    char *slash = strrchr(dir, '/');
    if (slash) *slash = 0;
    else snprintf(dir, sizeof dir, ".");
    assets = dir;
  }
  cw_jobs_init(0);
  cw_asset_set_root(assets); /* sub-scenes ([ext_scene]) load from the asset root */
  cw_scene_register_builtin_types();
  int rc = run(scene, assets, check);
  if (!watch) return rc;
  int64_t last = mtime_ns(scene);
  for (;;) {
    struct timespec ts = { 0, 250 * 1000000 };
    nanosleep(&ts, NULL);
    int64_t m = mtime_ns(scene);
    if (m != last && m >= 0) {
      last = m;
      run(scene, assets, false);
      fflush(stdout);
    }
  }
}

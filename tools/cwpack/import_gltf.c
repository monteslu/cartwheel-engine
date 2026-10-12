/* glTF importer: any glTF 2.0 (.gltf + .bin, or .glb) in, one GLB out that
 * three.c's GLTFLoader (behind the engine's loader) reads.
 *
 *   mesh.optimize   true: per primitive vertex-cache and overdraw order,
 *                   then vertex-fetch order (unused vertices dropped) over
 *                   the vertex buffer the primitives share
 *   mesh.lods       "0.5 0.25": simplified index buffers at those triangle
 *                   ratios (meshopt_simplify), listed in the primitive's
 *                   extras.cw_lods; "" for none
 *   mesh.lod_error  0.02: the simplifier's error bound (relative to extent)
 *   mesh.compress   false: EXT_meshopt_compression on vertex and index data
 *                   (the engine decodes it at load)
 *   texture.*       embedded images (see import_texture.c)
 *
 * Everything else in the file passes through untouched (materials,
 * animations, skins, extensions), external images stay external (cwpack
 * imports them as their own resources, shared with anything else using
 * them) and external buffers are folded into the GLB. */
#include <pthread.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>

#include "cwpack.h"
#include "gltf_doc.h"
#include "meshoptimizer.h"

#define CW_MESH_MAX_LODS_IMPORT 7 /* cartwheel/resource.h CW_MESH_MAX_LODS - 1 */

typedef struct {
  cwj *acc;
  const uint8_t *data;
  size_t stride, es;
  int count;
} acc_view;

static bool acc_resolve(cw_gltf_doc *d, cwj *accessors, int ai, acc_view *o) {
  memset(o, 0, sizeof *o);
  cwj *a = cwj_at(accessors, ai);
  if (!a || cwj_get(a, "sparse")) return false;
  int bv = (int)cwj_int(a, "bufferView", -1);
  if (bv < 0 || bv >= d->view_count || !d->views[bv].data) return false;
  o->acc = a;
  o->es = (size_t)cw_gltf_accessor_elem_size(a);
  if (!o->es) return false;
  cwj *view = cwj_at(cwj_get(d->root, "bufferViews"), bv);
  size_t stride = (size_t)cwj_int(view, "byteStride", 0);
  o->stride = stride ? stride : o->es;
  o->count = (int)cwj_int(a, "count", 0);
  size_t off = (size_t)cwj_int(a, "byteOffset", 0);
  if (o->count && off + (size_t)(o->count - 1) * o->stride + o->es > d->views[bv].len) return false;
  o->data = d->views[bv].data + off;
  return true;
}

static double comp_value(const uint8_t *p, int ct) {
  switch (ct) {
  case 5120: return *(const int8_t *)p;
  case 5121: return *p;
  case 5122: { int16_t v; memcpy(&v, p, 2); return v; }
  case 5123: { uint16_t v; memcpy(&v, p, 2); return v; }
  case 5125: { uint32_t v; memcpy(&v, p, 4); return v; }
  default: { float v; memcpy(&v, p, 4); return v; }
  }
}

/* recompute accessor min/max over tight data (vertices may have been dropped) */
static void recompute_bounds(cwj *a, const uint8_t *data, int count, size_t es) {
  if (!cwj_get(a, "min") && !cwj_get(a, "max")) return;
  int ct = (int)cwj_int(a, "componentType", 5126);
  int cs = ct == 5120 || ct == 5121 ? 1 : ct == 5122 || ct == 5123 ? 2 : 4;
  int nc = (int)(es / (size_t)cs);
  if (nc > 16 || !count) return;
  double mn[16], mx[16];
  for (int c = 0; c < nc; c++) { mn[c] = 1e300; mx[c] = -1e300; }
  for (int i = 0; i < count; i++)
    for (int c = 0; c < nc; c++) {
      double v = comp_value(data + (size_t)i * es + (size_t)c * cs, ct);
      if (v < mn[c]) mn[c] = v;
      if (v > mx[c]) mx[c] = v;
    }
  cwj *jmin = cwj_arr(), *jmax = cwj_arr();
  for (int c = 0; c < nc; c++) {
    cwj_push(jmin, ct == 5126 ? cwj_numf((float)mn[c]) : cwj_num(mn[c]));
    cwj_push(jmax, ct == 5126 ? cwj_numf((float)mx[c]) : cwj_num(mx[c]));
  }
  cwj_set(a, "min", jmin);
  cwj_set(a, "max", jmax);
}

typedef struct {
  char *sig;
  int *acc;       /* vertex accessors (attributes + morph targets) */
  int acc_n;
  int *mesh, *prim;
  int prim_n;
  bool ok;
} group;

static void add_int(int **a, int *n, int v) {
  *a = cw_realloc(*a, (size_t)(*n + 1) * sizeof **a, CW_MEM_GENERAL);
  (*a)[(*n)++] = v;
}

static void collect_attrs(cwj *attrs, char *sig, size_t cap, int **acc, int *n) {
  for (int i = 0; attrs && i < attrs->count; i++) {
    int ai = (int)cwj_num_or(attrs->items[i], -1);
    size_t k = strlen(sig);
    snprintf(sig + k, cap - k, "%s=%d;", attrs->keys[i], ai);
    if (acc) add_int(acc, n, ai);
  }
}

/* new index accessor data: u16 when it fits */
static int write_indices(cw_gltf_doc *d, cwj *acc, const uint32_t *ix, size_t n, size_t nv, bool compress) {
  bool u16 = nv <= 65535;
  size_t bytes = n * (u16 ? 2 : 4);
  uint8_t *b = cw_alloc(bytes ? bytes : 1, CW_MEM_GENERAL);
  for (size_t i = 0; i < n; i++) {
    if (u16) ((uint16_t *)b)[i] = (uint16_t)ix[i];
    else ((uint32_t *)b)[i] = ix[i];
  }
  int v = cw_gltf_doc_add_view(d, b, bytes, 0, 34963);
  if (compress && n % 3 == 0) {
    d->views[v].compress = CW_MESHOPT_TRIANGLES;
    d->views[v].count = (int)n;
    d->views[v].stride = u16 ? 2 : 4;
  }
  cwj_set(acc, "bufferView", cwj_num(v));
  cwj_remove(acc, "byteOffset");
  cwj_set(acc, "componentType", cwj_num(u16 ? 5123 : 5125));
  cwj_set(acc, "count", cwj_num((double)n));
  cwj_set(acc, "type", cwj_str("SCALAR"));
  cwj_remove(acc, "min");
  cwj_remove(acc, "max");
  cwj_remove(acc, "normalized");
  return v;
}

static int new_accessor(cwj *root) {
  cwj *accs = cwj_get(root, "accessors");
  cwj_push(accs, cwj_obj());
  return accs->count - 1;
}

static void parse_lods(const char *s, float *r, int *n) {
  *n = 0;
  while (s && *s && *n < CW_MESH_MAX_LODS_IMPORT) {
    char *e;
    double v = strtod(s, &e);
    if (e == s) { s++; continue; }
    if (v > 0 && v < 1) r[(*n)++] = (float)v;
    s = e;
  }
}

static void process_meshes(cw_gltf_doc *d, const cwp_settings *set, cwp_gltf_stats *st) {
  cwj *root = d->root, *meshes = cwj_get(root, "meshes"), *accessors = cwj_get(root, "accessors");
  if (!meshes || !accessors) return;
  bool optimize = cwp_settings_bool(set, "mesh.optimize", true);
  bool compress = cwp_settings_bool(set, "mesh.compress", false);
  float ratios[CW_MESH_MAX_LODS_IMPORT];
  int nlods;
  parse_lods(cwp_settings_get(set, "mesh.lods", "0.5 0.25"), ratios, &nlods);
  float lod_error = (float)cwp_settings_num(set, "mesh.lod_error", 0.02);
  /* group primitives by their vertex streams */
  group *g = NULL;
  int gn = 0;
  for (int m = 0; m < meshes->count; m++) {
    cwj *prims = cwj_get(meshes->items[m], "primitives");
    for (int p = 0; prims && p < prims->count; p++) {
      cwj *pr = prims->items[p];
      st->prims++;
      if (cwj_int(pr, "mode", 4) != 4) continue;
      if (cwj_get(cwj_get(pr, "extensions"), "KHR_draco_mesh_compression")) continue;
      char sig[4096] = "";
      collect_attrs(cwj_get(pr, "attributes"), sig, sizeof sig, NULL, NULL);
      cwj *targets = cwj_get(pr, "targets");
      for (int t = 0; targets && t < targets->count; t++) {
        strncat(sig, "|", sizeof sig - strlen(sig) - 1);
        collect_attrs(targets->items[t], sig, sizeof sig, NULL, NULL);
      }
      int gi = -1;
      for (int i = 0; i < gn; i++)
        if (!strcmp(g[i].sig, sig)) gi = i;
      if (gi < 0) {
        g = cw_realloc(g, (size_t)(gn + 1) * sizeof *g, CW_MEM_GENERAL);
        memset(&g[gn], 0, sizeof *g);
        g[gn].sig = cw_strdup(sig, CW_MEM_GENERAL);
        g[gn].ok = true;
        char tmp[4096] = "";
        collect_attrs(cwj_get(pr, "attributes"), tmp, sizeof tmp, &g[gn].acc, &g[gn].acc_n);
        for (int t = 0; targets && t < targets->count; t++) collect_attrs(targets->items[t], tmp, sizeof tmp, &g[gn].acc, &g[gn].acc_n);
        gi = gn++;
      }
      add_int(&g[gi].mesh, &g[gi].prim_n, m);
      g[gi].prim_n--;
      add_int(&g[gi].prim, &g[gi].prim_n, p);
    }
  }
  /* an accessor in two groups would be reordered twice: leave those groups */
  int *owner = cw_alloc((size_t)accessors->count * sizeof *owner + 4, CW_MEM_GENERAL);
  for (int i = 0; i < accessors->count; i++) owner[i] = -1;
  for (int i = 0; i < gn; i++)
    for (int k = 0; k < g[i].acc_n; k++) {
      int a = g[i].acc[k];
      if (a < 0 || a >= accessors->count) { g[i].ok = false; continue; }
      if (owner[a] >= 0 && owner[a] != i) { g[i].ok = false; g[owner[a]].ok = false; }
      owner[a] = i;
    }
  int *index_users = cw_calloc((size_t)accessors->count + 1, sizeof *index_users, CW_MEM_GENERAL);
  for (int m = 0; m < meshes->count; m++) {
    cwj *prims = cwj_get(meshes->items[m], "primitives");
    for (int p = 0; prims && p < prims->count; p++) {
      int ia = (int)cwj_int(prims->items[p], "indices", -1);
      if (ia >= 0 && ia < accessors->count) index_users[ia]++;
    }
  }
  for (int gi = 0; gi < gn; gi++) {
    group *G = &g[gi];
    if (!G->ok) continue;
    cwj *prim0 = cwj_at(cwj_get(meshes->items[G->mesh[0]], "primitives"), G->prim[0]);
    int pos_ai = (int)cwj_int(cwj_get(prim0, "attributes"), "POSITION", -1);
    acc_view pos;
    if (!acc_resolve(d, accessors, pos_ai, &pos) || cwj_int(pos.acc, "componentType", 0) != 5126 || pos.es != 12) continue;
    int nv = pos.count;
    acc_view *av = cw_alloc((size_t)G->acc_n * sizeof *av + 1, CW_MEM_GENERAL);
    bool ok = nv > 0;
    for (int k = 0; k < G->acc_n && ok; k++) ok = acc_resolve(d, accessors, G->acc[k], &av[k]) && av[k].count == nv;
    /* indices of every primitive, as u32 */
    uint32_t **ix = cw_calloc((size_t)G->prim_n, sizeof *ix, CW_MEM_GENERAL);
    size_t *ixn = cw_calloc((size_t)G->prim_n, sizeof *ixn, CW_MEM_GENERAL);
    for (int q = 0; q < G->prim_n && ok; q++) {
      cwj *pr = cwj_at(cwj_get(meshes->items[G->mesh[q]], "primitives"), G->prim[q]);
      int ia = (int)cwj_int(pr, "indices", -1);
      if (ia >= 0) {
        acc_view iv;
        if (!acc_resolve(d, accessors, ia, &iv) || !cwj_string(iv.acc, "type") || strcmp(cwj_string(iv.acc, "type"), "SCALAR")) { ok = false; break; }
        int ct = (int)cwj_int(iv.acc, "componentType", 0);
        ixn[q] = (size_t)iv.count;
        ix[q] = cw_alloc(ixn[q] * 4 + 4, CW_MEM_GENERAL);
        for (size_t i = 0; i < ixn[q]; i++) {
          uint32_t v = (uint32_t)comp_value(iv.data + i * iv.stride, ct);
          if (v >= (uint32_t)nv) { ok = false; break; }
          ix[q][i] = v;
        }
      } else {
        ixn[q] = (size_t)nv;
        ix[q] = cw_alloc(ixn[q] * 4 + 4, CW_MEM_GENERAL);
        for (size_t i = 0; i < ixn[q]; i++) ix[q][i] = (uint32_t)i;
      }
      if (ixn[q] % 3) ok = false;
    }
    if (!ok) goto next;
    float *P = cw_alloc((size_t)nv * 12, CW_MEM_GENERAL);
    for (int i = 0; i < nv; i++) memcpy(P + i * 3, pos.data + (size_t)i * pos.stride, 12);
    size_t total = 0;
    for (int q = 0; q < G->prim_n; q++) total += ixn[q];
    uint32_t *remap = cw_alloc((size_t)nv * 4, CW_MEM_GENERAL);
    size_t newv = (size_t)nv;
    if (optimize) {
      for (int q = 0; q < G->prim_n; q++) {
        meshopt_optimizeVertexCache(ix[q], ix[q], ixn[q], (size_t)nv);
        meshopt_optimizeOverdraw(ix[q], ix[q], ixn[q], P, (size_t)nv, 12, 1.05f);
      }
      uint32_t *all = cw_alloc(total * 4 + 4, CW_MEM_GENERAL);
      size_t k = 0;
      for (int q = 0; q < G->prim_n; q++) { memcpy(all + k, ix[q], ixn[q] * 4); k += ixn[q]; }
      newv = meshopt_optimizeVertexFetchRemap(remap, all, total, (size_t)nv);
      cw_free(all);
    } else {
      for (int i = 0; i < nv; i++) remap[i] = (uint32_t)i;
    }
    st->vertices_in += nv;
    st->vertices_out += (int)newv;
    /* vertex streams in the new order, one tight view each */
    for (int k = 0; k < G->acc_n && optimize; k++) {
      size_t es = av[k].es;
      uint8_t *nb = cw_calloc(newv ? newv : 1, es, CW_MEM_GENERAL);
      for (int i = 0; i < nv; i++)
        if (remap[i] != ~0u) memcpy(nb + (size_t)remap[i] * es, av[k].data + (size_t)i * av[k].stride, es);
      int v = cw_gltf_doc_add_view(d, nb, newv * es, 0, 34962);
      if (compress && es % 4 == 0 && es <= 256) {
        d->views[v].compress = CW_MESHOPT_ATTRIBUTES;
        d->views[v].count = (int)newv;
        d->views[v].stride = (int)es;
        st->compressed_views++;
      }
      cwj *acc = av[k].acc;
      cwj_set(acc, "bufferView", cwj_num(v));
      cwj_remove(acc, "byteOffset");
      cwj_set(acc, "count", cwj_num((double)newv));
      recompute_bounds(acc, nb, (int)newv, es);
    }
    float *NP = P;
    if (optimize) {
      NP = cw_alloc(newv * 12 + 12, CW_MEM_GENERAL);
      for (int i = 0; i < nv; i++)
        if (remap[i] != ~0u) memcpy(NP + (size_t)remap[i] * 3, P + i * 3, 12);
      for (int q = 0; q < G->prim_n; q++) meshopt_remapIndexBuffer(ix[q], ix[q], ixn[q], remap);
    }
    for (int q = 0; q < G->prim_n; q++) {
      cwj *pr = cwj_at(cwj_get(meshes->items[G->mesh[q]], "primitives"), G->prim[q]);
      int ia = (int)cwj_int(pr, "indices", -1);
      if (optimize) {
        if (ia < 0 || index_users[ia] > 1) { /* none, or shared with another primitive: a new accessor */
          if (ia >= 0) index_users[ia]--;
          ia = new_accessor(d->root);
          cwj_set(pr, "indices", cwj_num(ia));
        }
        write_indices(d, cwj_at(accessors, ia), ix[q], ixn[q], newv, compress);
        if (compress) st->compressed_views++;
        st->prims_optimized++;
      }
      st->triangles += (int)(ixn[q] / 3);
      /* LODs */
      cwj *list = cwj_arr();
      size_t prev = ixn[q];
      uint32_t *lod = cw_alloc(ixn[q] * 4 + 4, CW_MEM_GENERAL);
      for (int l = 0; l < nlods; l++) {
        size_t target = (size_t)((double)ixn[q] * ratios[l]) / 3 * 3;
        if (target < 3) break;
        float err = 0;
        size_t n = meshopt_simplify(lod, ix[q], ixn[q], NP, newv, 12, target, lod_error, 0, &err);
        if (!n || n > prev * 9 / 10) break; /* no real reduction left within the error bound */
        meshopt_optimizeVertexCache(lod, lod, n, newv);
        int la = new_accessor(d->root);
        write_indices(d, cwj_at(accessors, la), lod, n, newv, compress);
        cwj *e = cwj_obj();
        cwj_set(e, "indices", cwj_num(la));
        cwj_set(e, "error", cwj_numf(err));
        cwj_push(list, e);
        st->lods++;
        st->lod_triangles += (int)(n / 3);
        prev = n;
      }
      cw_free(lod);
      if (list->count) {
        cwj *ex = cwj_get(pr, "extras");
        if (!ex || ex->type != CWJ_OBJ) { ex = cwj_obj(); cwj_set(pr, "extras", ex); }
        cwj_set(ex, "cw_lods", list);
      } else {
        cwj_free(list);
      }
    }
    if (NP != P) cw_free(NP);
    cw_free(P);
    cw_free(remap);
  next:
    for (int q = 0; q < G->prim_n; q++) cw_free(ix[q]);
    cw_free(ix);
    cw_free(ixn);
    cw_free(av);
  }
  for (int i = 0; i < gn; i++) { cw_free(g[i].sig); cw_free(g[i].acc); cw_free(g[i].mesh); cw_free(g[i].prim); }
  cw_free(g);
  cw_free(owner);
  cw_free(index_users);
}

typedef struct img_job {
  int image;
  char name[CWP_PATH_MAX + 32];
  const uint8_t *src;
  size_t len;
  const cwp_settings *set;
  bool srgb;
  uint8_t *out;
  size_t out_len;
} img_job;

static void *img_thread(void *arg) {
  img_job *j = arg;
  j->out = cwp_import_texture(j->name, j->src, j->len, j->set, j->srgb, &j->out_len);
  return NULL;
}

/* at most as many threads as cores at a time */
static void run_jobs(img_job *jobs, int n) {
  long cores = sysconf(_SC_NPROCESSORS_ONLN);
  int width = cores > 0 ? (int)cores : 1;
  if (getenv("CWPACK_THREADS")) width = CW_MAX(1, atoi(getenv("CWPACK_THREADS"))); /* 1 = serial (tests) */
  for (int b = 0; b < n; b += width) {
    pthread_t th[256];
    int m = CW_MIN(width, CW_MIN(n - b, 256));
    bool started[256] = { false };
    for (int i = 0; i < m; i++) started[i] = pthread_create(&th[i], NULL, img_thread, &jobs[b + i]) == 0;
    for (int i = 0; i < m; i++) {
      if (started[i]) pthread_join(th[i], NULL);
      else img_thread(&jobs[b + i]);
    }
  }
}

/* embedded images -> KTX2 (texture.format = ktx2) */
static void process_images(cw_gltf_doc *d, const char *name, const cwp_settings *set, cwp_gltf_stats *st) {
  if (strcmp(cwp_settings_get(set, "texture.format", "keep"), "ktx2")) return;
  cwj *root = d->root, *images = cwj_get(root, "images"), *textures = cwj_get(root, "textures"),
      *materials = cwj_get(root, "materials");
  if (!images || !textures) return;
  /* linear data: normal, occlusion, metallic-roughness maps */
  bool *linear = cw_calloc((size_t)images->count + 1, 1, CW_MEM_GENERAL);
  for (int m = 0; materials && m < materials->count; m++) {
    cwj *mat = materials->items[m];
    cwj *refs[3] = { cwj_get(mat, "normalTexture"), cwj_get(mat, "occlusionTexture"),
                     cwj_get(cwj_get(mat, "pbrMetallicRoughness"), "metallicRoughnessTexture") };
    for (int r = 0; r < 3; r++) {
      int t = (int)cwj_int(refs[r], "index", -1);
      int img = (int)cwj_int(cwj_at(textures, t), "source", -1);
      if (img >= 0 && img < images->count) linear[img] = true;
    }
  }
  bool any = false;
  /* encode every image at once, one thread each (each encode is itself
   * single-threaded, so the bytes do not depend on the thread count), then
   * apply the results in image order */
  img_job *jobs = cw_calloc((size_t)images->count + 1, sizeof *jobs, CW_MEM_GENERAL);
  int nj = 0;
  for (int i = 0; i < images->count; i++) {
    cwj *im = images->items[i];
    int bv = (int)cwj_int(im, "bufferView", -1);
    const char *mime = cwj_string(im, "mimeType");
    if (bv < 0 || bv >= d->view_count || (mime && !strcmp(mime, "image/ktx2"))) continue;
    img_job *j = &jobs[nj++];
    j->image = i;
    snprintf(j->name, sizeof j->name, "%s#image/%d", name, i);
    j->src = d->views[bv].data;
    j->len = d->views[bv].len;
    j->set = set;
    j->srgb = !linear[i];
  }
  cwp_basis_init();
  run_jobs(jobs, nj);
  for (int q = 0; q < nj; q++) {
    cwj *im = images->items[jobs[q].image];
    uint8_t *k = jobs[q].out;
    size_t n = jobs[q].out_len;
    if (!k) continue;
    int v = cw_gltf_doc_add_view(d, k, n, 0, 0);
    cwj_set(im, "bufferView", cwj_num(v));
    cwj_set(im, "mimeType", cwj_str("image/ktx2"));
    st->images_ktx2++;
    any = true;
  }
  cw_free(jobs);
  if (any) {
    for (int t = 0; t < textures->count; t++) {
      cwj *tx = textures->items[t];
      int img = (int)cwj_int(tx, "source", -1);
      const char *mime = cwj_string(cwj_at(images, img), "mimeType");
      if (!mime || strcmp(mime, "image/ktx2")) continue;
      cwj *ext = cwj_get(tx, "extensions");
      if (!ext) { ext = cwj_obj(); cwj_set(tx, "extensions", ext); }
      cwj *b = cwj_obj();
      cwj_set(b, "source", cwj_num(img));
      cwj_set(ext, "KHR_texture_basisu", b);
      cwj_remove(tx, "source");
    }
    for (int pass = 0; pass < 2; pass++) {
      const char *key = pass ? "extensionsRequired" : "extensionsUsed";
      cwj *list = cwj_get(root, key);
      if (!list) { list = cwj_arr(); cwj_set(root, key, list); }
      bool has = false;
      for (int i = 0; i < list->count; i++) has |= list->items[i]->type == CWJ_STR && !strcmp(list->items[i]->text, "KHR_texture_basisu");
      if (!has) cwj_push(list, cwj_str("KHR_texture_basisu"));
    }
  }
  cw_free(linear);
}

/* every accessor a primitive reads must lie inside its bufferView, and every
 * index a primitive names must exist: a broken file fails here with a message
 * naming the accessor, instead of importing to nothing */
static bool validate(cw_gltf_doc *d, char *err, size_t cap) {
  cwj *accs = cwj_get(d->root, "accessors"), *meshes = cwj_get(d->root, "meshes"), *mats = cwj_get(d->root, "materials");
  int nacc = accs ? accs->count : 0;
  for (int i = 0; i < nacc; i++) {
    cwj *a = accs->items[i];
    int bv = (int)cwj_int(a, "bufferView", -1);
    if (bv < 0 || cwj_get(a, "sparse")) continue; /* zero-filled or sparse: nothing to bound here */
    if (bv >= d->view_count) { snprintf(err, cap, "accessor %d: bufferView %d does not exist (%d views)", i, bv, d->view_count); return false; }
    if (!d->views[bv].data) continue; /* a compressed fallback view (decoded later) */
    size_t es = (size_t)cw_gltf_accessor_elem_size(a);
    if (!es) { snprintf(err, cap, "accessor %d: unknown type / componentType", i); return false; }
    cwj *view = cwj_at(cwj_get(d->root, "bufferViews"), bv);
    size_t stride = (size_t)cwj_int(view, "byteStride", 0);
    if (!stride) stride = es;
    long long count = cwj_int(a, "count", 0);
    size_t off = (size_t)cwj_int(a, "byteOffset", 0);
    if (count < 0) { snprintf(err, cap, "accessor %d: negative count", i); return false; }
    size_t need = count ? off + (size_t)(count - 1) * stride + es : off;
    if (need > d->views[bv].len) {
      snprintf(err, cap, "accessor %d reads past its data: %lld elements of %zu bytes from byteOffset %zu need %zu bytes, "
                         "bufferView %d has %zu (the file is truncated or its counts are wrong)",
               i, count, es, off, need, bv, d->views[bv].len);
      return false;
    }
  }
  for (int m = 0; meshes && m < meshes->count; m++) {
    cwj *ps = cwj_get(meshes->items[m], "primitives");
    for (int q = 0; ps && q < ps->count; q++) {
      cwj *pr = ps->items[q], *at = cwj_get(pr, "attributes");
      if (!at || !cwj_get(at, "POSITION")) { snprintf(err, cap, "mesh %d primitive %d has no POSITION attribute", m, q); return false; }
      for (int k = 0; k < at->count; k++) {
        int ai = (int)at->items[k]->num;
        if (ai < 0 || ai >= nacc) { snprintf(err, cap, "mesh %d primitive %d: attribute %s names accessor %d (there are %d)", m, q, at->keys[k], ai, nacc); return false; }
      }
      int ii = (int)cwj_int(pr, "indices", -1), mi = (int)cwj_int(pr, "material", -1);
      if (cwj_get(pr, "indices") && (ii < 0 || ii >= nacc)) { snprintf(err, cap, "mesh %d primitive %d: indices name accessor %d (there are %d)", m, q, ii, nacc); return false; }
      if (cwj_get(pr, "material") && (mi < 0 || !mats || mi >= mats->count)) { snprintf(err, cap, "mesh %d primitive %d: material %d does not exist", m, q, mi); return false; }
    }
  }
  return true;
}

uint8_t *cwp_import_gltf(const char *name, const uint8_t *src, size_t len, cwp_read_fn read, void *ctx,
                         const cwp_settings *set, size_t *out_len, cwp_gltf_stats *st) {
  char err[512];
  cw_gltf_doc d;
  memset(st, 0, sizeof *st);
  if (!cw_gltf_doc_open(&d, src, len, (cw_gltf_read_fn)read, ctx, err, sizeof err)) {
    cwp_report("%s: %s", name, err);
    return NULL;
  }
  if (!cw_gltf_doc_meshopt_decode(&d, err, sizeof err) || !validate(&d, err, sizeof err)) {
    cwp_report("%s: %s", name, err);
    cw_gltf_doc_free(&d);
    return NULL;
  }
  /* the version of this importer, for anyone reading the output */
  cwj *asset = cwj_get(d.root, "asset");
  if (asset) {
    cwj_set(asset, "generator", cwj_str("cartwheel cwpack"));
  }
  process_meshes(&d, set, st);
  process_images(&d, name, set, st);
  uint8_t *out = cw_gltf_doc_write_glb(&d, out_len);
  cw_gltf_doc_free(&d);
  return out;
}

bool cwp_gltf_subresources(const uint8_t *src, size_t len, void (*cb)(const char *name, const char *type, void *user),
                           void (*file_cb)(const char *uri, void *user), void *user) {
  const char *json;
  size_t jl, bl;
  const uint8_t *bin;
  cwj *r = cw_glb_split(src, len, &json, &jl, &bin, &bl) ? cwj_parse(json, jl, NULL, 0) : NULL;
  if (!r || !cwj_get(r, "asset")) {
    cwj_free(r);
    return false;
  }
  char sub[64];
  cwj *ms = cwj_get(r, "meshes"), *mats = cwj_get(r, "materials"), *txs = cwj_get(r, "textures");
  for (int m = 0; cb && ms && m < ms->count; m++) {
    cwj *ps = cwj_get(ms->items[m], "primitives");
    for (int q = 0; ps && q < ps->count; q++) {
      snprintf(sub, sizeof sub, "mesh/%d/%d", m, q);
      cb(sub, "mesh", user);
    }
  }
  for (int i = 0; cb && mats && i < mats->count; i++) { snprintf(sub, sizeof sub, "material/%d", i); cb(sub, "material", user); }
  for (int i = 0; cb && txs && i < txs->count; i++) { snprintf(sub, sizeof sub, "texture/%d", i); cb(sub, "texture", user); }
  const char *lists[] = { "buffers", "images" };
  for (int l = 0; file_cb && l < 2; l++) {
    cwj *a = cwj_get(r, lists[l]);
    for (int i = 0; a && i < a->count; i++) {
      const char *u = cwj_string(a->items[i], "uri");
      if (u && strncmp(u, "data:", 5)) file_cb(u, user);
    }
  }
  cwj_free(r);
  return true;
}

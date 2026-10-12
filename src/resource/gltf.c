/* glTF scene resources over three.c's GLTFLoader.
 *
 * Decode (worker): open the document, decode EXT_meshopt_compression, decode
 * every embedded image (PNG/JPEG/KTX2) to its GPU form, and rewrite the GLB
 * so each glTF texture names a placeholder image ("cwimg:<texture>"); read
 * cwpack's LOD index buffers and a matching key per primitive.
 * Finish (main thread): three.c builds the scene from the rewritten GLB (its
 * read callback answers each placeholder with a 1x1 PNG whose pixel encodes
 * the texture index), then each placeholder texture is filled in place with
 * the decoded image (or swapped for an external image's texture resource),
 * LOD geometries are built sharing the primitive's attributes, and meshes,
 * materials and textures are registered as sub-resources. */
#include <stdio.h>
#include <stdlib.h>

#include "cartwheel/gl.h"
#include "cgltf.h"
#include "gltf_doc.h"
#include "resource_internal.h"
#include "three.h"

#define PH_B 0xC7
#define PH_A 0x3A

typedef struct prim_rec {
  int mesh, prim, material;
  int vcount, icount;
  uint64_t key; /* hash of the position floats, as three.c unpacks them */
  bool used;
  int lod_count; /* extra LODs (lod 0 is the primitive) */
  uint32_t *lod_idx[CW_MESH_MAX_LODS];
  int lod_n[CW_MESH_MAX_LODS];
  float lod_err[CW_MESH_MAX_LODS];
} prim_rec;

typedef struct gltf_dec {
  uint8_t *glb;
  size_t glb_len;
  int tex_count;
  int *tex_image;     /* per glTF texture: its image */
  int *image_ext;     /* per image: external dependency index, -1 = embedded */
  bool *tex_private;  /* per glTF texture: its external image was read privately: its own upload */
  bool *image_private; /* per image: external, read privately (custom sampler, or another colour space) */
  cw_tex_cpu *images; /* per image: decoded (embedded ones) */
  bool *image_ok;
  int image_count;
  prim_rec *prims;
  int prim_count;
  int material_count, mesh_count;
} gltf_dec;

typedef struct gltf_payload {
  cw_gltf pub; /* first: cw_res_gltf returns it */
  cw_res self;
  cw_mesh **meshes; /* one per matched primitive */
  char **mesh_names;
  int mesh_n;
  t3_material **materials; /* per glTF material (NULL: unused) */
  int material_n;
  cw_tex_payload **textures; /* per glTF texture (NULL: unused) */
  int texture_n;
  CW_ARRAY(t3_object *) instances;
} gltf_payload;

/* geometry -> its cw_mesh (any LOD), instance root -> scene slot / LOD */
static cw_map geo_map, inst_map, inst_lod;
static bool maps_init;
static void ensure_maps(void) {
  if (maps_init) return;
  maps_init = true;
  cw_map_init(&geo_map, CW_MEM_RESOURCE);
  cw_map_init(&inst_map, CW_MEM_RESOURCE);
  cw_map_init(&inst_lod, CW_MEM_RESOURCE);
}
void cw__gltf_shutdown(void) {
  if (!maps_init) return;
  cw_map_free(&geo_map);
  cw_map_free(&inst_map);
  cw_map_free(&inst_lod);
  maps_init = false;
}
#define PKEY(p) ((uint64_t)(uintptr_t)(p))

/* ── helpers ─────────────────────────────────────────────────────── */
static void dir_of(const char *path, char *dir, size_t cap) {
  const char *s = strrchr(path, '/');
  snprintf(dir, cap, "%.*s", s ? (int)(s - path + 1) : 0, path);
}
static void uri_decode(const char *s, char *o, size_t cap) {
  size_t k = 0;
  for (size_t i = 0; s[i] && k + 1 < cap; i++) {
    if (s[i] == '%' && s[i + 1] && s[i + 2]) {
      char h[3] = { s[i + 1], s[i + 2], 0 };
      o[k++] = (char)strtol(h, NULL, 16);
      i += 2;
    } else o[k++] = s[i];
  }
  o[k] = 0;
}
static bool is_external(const char *uri) { return uri && strncmp(uri, "data:", 5) && strncmp(uri, "cwimg:", 6); }

/* the image a texture shows: KHR_texture_basisu's source when present */
static int texture_source(const cwj *tex) {
  cwj *b = cwj_get(cwj_get(tex, "extensions"), "KHR_texture_basisu");
  if (b) return (int)cwj_int(b, "source", -1);
  return (int)cwj_int(tex, "source", -1);
}

/* A texture whose glTF sampler is not the default one (repeat, linear,
 * trilinear: what texture resources use) cannot share the external image's
 * texture resource, whose GL sampling state is its own: it gets a private
 * upload of the same file. */
static bool sampler_custom(const cwj *root, const cwj *tex) {
  int si = (int)cwj_int(tex, "sampler", -1);
  const cwj *sm = cwj_at(cwj_get(root, "samplers"), si);
  if (!sm) return false;
  int mag = (int)cwj_int(sm, "magFilter", 9729), min = (int)cwj_int(sm, "minFilter", 9987);
  int ws = (int)cwj_int(sm, "wrapS", 10497), wt = (int)cwj_int(sm, "wrapT", 10497);
  return mag != 9729 || min != 9987 || ws != 10497 || wt != 10497;
}
/* A texture a material shows as colour (baseColorTexture, emissiveTexture:
 * sRGB, three.c's glTF loader marks them so). */
static bool texture_srgb(const cwj *root, int tex) {
  const cwj *mats = cwj_get(root, "materials");
  for (int i = 0; mats && i < mats->count; i++) {
    const cwj *m = mats->items[i];
    if (cwj_int(cwj_get(cwj_get(m, "pbrMetallicRoughness"), "baseColorTexture"), "index", -1) == tex ||
        cwj_int(cwj_get(m, "emissiveTexture"), "index", -1) == tex)
      return true;
  }
  return false;
}
static bool image_sampler_private(const cwj *root, int img) {
  const cwj *texs = cwj_get(root, "textures");
  for (int t = 0; texs && t < texs->count; t++)
    if (texture_source(texs->items[t]) == img && sampler_custom(root, texs->items[t])) return true;
  return false;
}
static bool image_srgb(const cwj *root, int img) {
  const cwj *texs = cwj_get(root, "textures");
  for (int t = 0; texs && t < texs->count; t++)
    if (texture_source(texs->items[t]) == img && texture_srgb(root, t)) return true;
  return false;
}

/* ── prepare: external buffers and images ────────────────────────── */
static bool gltf_prepare(cw_res_prepare *p, void *u) {
  size_t len;
  const uint8_t *bytes = cw_res_prepare_bytes(p, &len);
  const char *path = cw_res_prepare_path(p);
  const char *json;
  size_t jl, bl;
  const uint8_t *bin;
  if (!cw_glb_split(bytes, len, &json, &jl, &bin, &bl)) { cw_error("res://%s: not a glTF 2.0 file", path); return false; }
  char err[160], dir[CW_RES_PATH_MAX], full[CW_RES_PATH_MAX * 2], uri[CW_RES_PATH_MAX];
  cwj *root = cwj_parse(json, jl, err, sizeof err);
  if (!root) { cw_error("res://%s: glTF JSON %s", path, err); return false; }
  dir_of(path, dir, sizeof dir);
  bool ok = true;
  cwj *bufs = cwj_get(root, "buffers"), *imgs = cwj_get(root, "images");
  for (int i = 0; bufs && i < bufs->count && ok; i++) {
    const char *bu = cwj_string(bufs->items[i], "uri");
    if (!is_external(bu)) continue;
    uri_decode(bu, uri, sizeof uri);
    snprintf(full, sizeof full, "%s%s", dir, uri);
    if (!cw_res_prepare_read(p, full)) { cw_error("res://%s: buffer \"%s\" not found (res://%s)", path, bu, full); ok = false; }
  }
  for (int i = 0; imgs && i < imgs->count; i++) {
    const char *iu = cwj_string(imgs->items[i], "uri");
    if (!is_external(iu)) continue;
    uri_decode(iu, uri, sizeof uri);
    snprintf(full, sizeof full, "%s%s", dir, uri);
    /* its own copy for its textures when a sampler is custom, or when the
     * shared texture resource is not in the colour space this glTF shows the
     * image in (the colour space is fixed when the texels upload; claimed
     * before the dependency can load) */
    bool own = !cw__tex_res_claim(full, image_srgb(root, i)) || image_sampler_private(root, i);
    cw_res_prepare_dep(p, full, CW_RES_TEXTURE);
    if (own) cw_res_prepare_read(p, full);
  }
  /* embedded KTX2 decodes on a worker: fix the target first (main thread;
   * sniffing every image for KTX2 here would cost more than this) */
  cw__tex_pick_ktx2_target();
  cwj_free(root);
  return ok;
}

/* ── decode (worker) ─────────────────────────────────────────────── */
typedef struct { const cw_res_decode_args *a; char dir[CW_RES_PATH_MAX]; } read_ctx;
static void *read_extra(void *ctx, const char *uri, size_t *len) {
  read_ctx *rc = ctx;
  char full[CW_RES_PATH_MAX * 2], canon[CW_RES_PATH_MAX];
  snprintf(full, sizeof full, "%s%s", rc->dir, uri);
  if (!cw__res_canon(full, canon, sizeof canon)) return NULL;
  for (int i = 0; i < rc->a->extra_count; i++)
    if (!strcmp(rc->a->extra_paths[i], canon)) {
      uint8_t *c = cw_alloc(rc->a->extra_lens[i] + 1, CW_MEM_RESOURCE);
      memcpy(c, rc->a->extra_bytes[i], rc->a->extra_lens[i]);
      *len = rc->a->extra_lens[i];
      return c;
    }
  return NULL;
}

static void dec_free(gltf_dec *d) {
  if (!d) return;
  cw_free(d->glb);
  for (int i = 0; i < d->image_count; i++) cw__tex_cpu_free(&d->images[i]);
  cw_free(d->images);
  cw_free(d->image_ok);
  cw_free(d->image_ext);
  cw_free(d->tex_image);
  cw_free(d->tex_private);
  cw_free(d->image_private);
  for (int i = 0; i < d->prim_count; i++)
    for (int l = 0; l < CW_MESH_MAX_LODS; l++) cw_free(d->prims[i].lod_idx[l]);
  cw_free(d->prims);
  cw_free(d);
}

static uint8_t *b64(const char *s, size_t *n) {
  size_t L = strlen(s);
  uint8_t *o = cw_alloc(L * 3 / 4 + 3, CW_MEM_RESOURCE);
  uint32_t acc = 0;
  int bits = 0;
  size_t k = 0;
  for (size_t i = 0; i < L; i++) {
    char c = s[i];
    int v = c >= 'A' && c <= 'Z' ? c - 'A' : c >= 'a' && c <= 'z' ? c - 'a' + 26 : c >= '0' && c <= '9' ? c - '0' + 52
          : c == '+' ? 62 : c == '/' ? 63 : -1;
    if (v < 0) continue;
    acc = acc << 6 | (uint32_t)v;
    bits += 6;
    if (bits >= 8) { bits -= 8; o[k++] = (uint8_t)(acc >> bits); }
  }
  *n = k;
  return o;
}

static void drop_ext_name(cwj *root, const char *list, const char *name) {
  cwj *a = cwj_get(root, list);
  if (!a || a->type != CWJ_ARR) return;
  for (int i = 0; i < a->count; i++)
    if (a->items[i]->type == CWJ_STR && !strcmp(a->items[i]->text, name)) {
      cwj_free(a->items[i]);
      memmove(a->items + i, a->items + i + 1, (size_t)(a->count - i - 1) * sizeof *a->items);
      a->count--;
      i--;
    }
  if (!a->count) cwj_remove(root, list);
}

static uint64_t hash_floats(const float *f, size_t n) { return cw_hash_bytes(f, n * sizeof *f, 0x6c6f64); }

static void *gltf_decode(const cw_res_decode_args *a, size_t *cost) {
  char err[200];
  read_ctx rc = { a, { 0 } };
  dir_of(a->path, rc.dir, sizeof rc.dir);
  cw_gltf_doc doc;
  if (!cw_gltf_doc_open(&doc, a->bytes, a->len, read_extra, &rc, err, sizeof err)) {
    cw_error("res://%s: %s", a->path, err);
    return NULL;
  }
  if (!cw_gltf_doc_meshopt_decode(&doc, err, sizeof err)) {
    cw_error("res://%s: %s", a->path, err);
    cw_gltf_doc_free(&doc);
    return NULL;
  }
  gltf_dec *d = cw_calloc(1, sizeof *d, CW_MEM_RESOURCE);
  cwj *root = doc.root;
  cwj *imgs = cwj_get(root, "images"), *texs = cwj_get(root, "textures");
  d->image_count = imgs ? imgs->count : 0;
  d->tex_count = texs ? texs->count : 0;
  d->images = cw_calloc((size_t)d->image_count + 1, sizeof *d->images, CW_MEM_RESOURCE);
  d->image_ok = cw_calloc((size_t)d->image_count + 1, sizeof *d->image_ok, CW_MEM_RESOURCE);
  d->image_ext = cw_calloc((size_t)d->image_count + 1, sizeof *d->image_ext, CW_MEM_RESOURCE);
  d->tex_image = cw_calloc((size_t)d->tex_count + 1, sizeof *d->tex_image, CW_MEM_RESOURCE);
  d->tex_private = cw_calloc((size_t)d->tex_count + 1, sizeof *d->tex_private, CW_MEM_RESOURCE);
  d->image_private = cw_calloc((size_t)d->image_count + 1, sizeof *d->image_private, CW_MEM_RESOURCE);
  int ext = 0;
  for (int i = 0; i < d->image_count; i++) {
    cwj *im = imgs->items[i];
    const char *uri = cwj_string(im, "uri");
    d->image_ext[i] = -1;
    const uint8_t *bytes = NULL;
    uint8_t *owned = NULL;
    size_t n = 0;
    if (is_external(uri)) {
      d->image_ext[i] = ext++;
      char dec[CW_RES_PATH_MAX];
      uri_decode(uri, dec, sizeof dec);
      bytes = owned = read_extra(&rc, dec, &n); /* prepare read it: its textures upload it themselves */
      if (!bytes) continue;
      d->image_private[i] = true;
    }
    int bv = (int)cwj_int(im, "bufferView", -1);
    if (bytes) {
    } else if (bv >= 0 && bv < doc.view_count) { bytes = doc.views[bv].data; n = doc.views[bv].len; }
    else if (uri && !strncmp(uri, "data:", 5) && strchr(uri, ',')) bytes = owned = b64(strchr(uri, ',') + 1, &n);
    char name[CW_RES_PATH_MAX + 32];
    snprintf(name, sizeof name, "%s#image/%d", a->path, i);
    if (bytes) d->image_ok[i] = cw__tex_decode(name, bytes, n, &d->images[i]);
    cw_free(owned);
    for (int l = 0; l < d->images[i].levels; l++) *cost += d->images[i].level[l].size;
  }
  /* one placeholder image per texture */
  cwj *nimgs = cwj_arr();
  for (int t = 0; t < d->tex_count; t++) {
    cwj *tx = texs->items[t];
    d->tex_image[t] = texture_source(tx);
    d->tex_private[t] = d->tex_image[t] >= 0 && d->tex_image[t] < d->image_count && d->image_private[d->tex_image[t]];
    cwj_remove(tx, "extensions");
    cwj_set(tx, "source", cwj_num(t));
    cwj *ni = cwj_obj();
    char u[32];
    snprintf(u, sizeof u, "cwimg:%d", t);
    cwj_set(ni, "uri", cwj_str(u));
    cwj_push(nimgs, ni);
  }
  if (d->tex_count) cwj_set(root, "images", nimgs);
  else { cwj_free(nimgs); cwj_remove(root, "images"); }
  drop_ext_name(root, "extensionsUsed", "KHR_texture_basisu");
  drop_ext_name(root, "extensionsRequired", "KHR_texture_basisu");
  /* LOD records come from the document (accessor indices survive the write) */
  cwj *meshes = cwj_get(root, "meshes");
  d->mesh_count = meshes ? meshes->count : 0;
  d->material_count = (int)(cwj_get(root, "materials") ? cwj_get(root, "materials")->count : 0);
  for (int m = 0; m < d->mesh_count; m++) {
    cwj *prims = cwj_get(meshes->items[m], "primitives");
    d->prim_count += prims ? prims->count : 0;
  }
  d->prims = cw_calloc((size_t)d->prim_count + 1, sizeof *d->prims, CW_MEM_RESOURCE);
  d->glb = cw_gltf_doc_write_glb(&doc, &d->glb_len);
  /* positions and LOD indices through cgltf, exactly as three.c reads them */
  cgltf_options opt = { 0 };
  cgltf_data *cd = NULL;
  if (cgltf_parse(&opt, d->glb, d->glb_len, &cd) != cgltf_result_success) {
    cw_error("res://%s: rewritten glTF does not parse (cwpack bug?)", a->path);
    cw_gltf_doc_free(&doc);
    dec_free(d);
    return NULL;
  }
  for (cgltf_size i = 0; i < cd->buffers_count; i++)
    if (!cd->buffers[i].data && !cd->buffers[i].uri && cd->bin) {
      cd->buffers[i].data = (void *)cd->bin;
      cd->buffers[i].data_free_method = cgltf_data_free_method_none;
    }
  int k = 0;
  for (int m = 0; m < d->mesh_count; m++) {
    cwj *prims = cwj_get(meshes->items[m], "primitives");
    for (int p = 0; prims && p < prims->count; p++, k++) {
      prim_rec *r = &d->prims[k];
      r->mesh = m;
      r->prim = p;
      r->material = (int)cwj_int(prims->items[p], "material", -1);
      const cgltf_primitive *cp = &cd->meshes[m].primitives[p];
      const cgltf_accessor *pos = NULL;
      for (cgltf_size q = 0; q < cp->attributes_count; q++)
        if (cp->attributes[q].type == cgltf_attribute_type_position) pos = cp->attributes[q].data;
      if (!pos) continue;
      r->vcount = (int)pos->count;
      r->icount = cp->indices ? (int)cp->indices->count : 0;
      float *f = cw_alloc(pos->count * 3 * sizeof *f + 4, CW_MEM_RESOURCE);
      cgltf_accessor_unpack_floats(pos, f, pos->count * 3);
      r->key = hash_floats(f, pos->count * 3);
      cw_free(f);
      cwj *lods = cwj_get(cwj_get(prims->items[p], "extras"), "cw_lods");
      for (int l = 0; lods && lods->type == CWJ_ARR && l < lods->count && r->lod_count < CW_MESH_MAX_LODS - 1; l++) {
        int ai = (int)cwj_int(lods->items[l], "indices", -1);
        if (ai < 0 || ai >= (int)cd->accessors_count) continue;
        const cgltf_accessor *ia = &cd->accessors[ai];
        int n = (int)ia->count;
        uint32_t *ix = cw_alloc((size_t)n * 4 + 4, CW_MEM_RESOURCE);
        for (int j = 0; j < n; j++) ix[j] = (uint32_t)cgltf_accessor_read_index(ia, (cgltf_size)j);
        int li = ++r->lod_count;
        r->lod_idx[li] = ix;
        r->lod_n[li] = n;
        r->lod_err[li] = (float)cwj_num_or(cwj_get(lods->items[l], "error"), 0);
        *cost += (size_t)n * 4;
      }
    }
  }
  cgltf_free(cd);
  cw_gltf_doc_free(&doc);
  *cost += d->glb_len;
  return d;
}

/* ── finish (main thread) ────────────────────────────────────────── */
static uint32_t crc_table[256];
static uint32_t crc32_of(const uint8_t *p, size_t n) {
  if (!crc_table[1])
    for (uint32_t i = 0; i < 256; i++) {
      uint32_t c = i;
      for (int k = 0; k < 8; k++) c = c & 1 ? 0xEDB88320u ^ (c >> 1) : c >> 1;
      crc_table[i] = c;
    }
  uint32_t c = 0xFFFFFFFFu;
  for (size_t i = 0; i < n; i++) c = crc_table[(c ^ p[i]) & 255] ^ (c >> 8);
  return c ^ 0xFFFFFFFFu;
}
static void be32(uint8_t *p, uint32_t v) { p[0] = (uint8_t)(v >> 24); p[1] = (uint8_t)(v >> 16); p[2] = (uint8_t)(v >> 8); p[3] = (uint8_t)v; }

/* A 1x1 RGBA PNG (stored deflate) whose pixel names texture t. */
static void *placeholder_png(void *ctx, const char *uri, size_t *len) {
  if (strncmp(uri, "cwimg:", 6)) return NULL;
  int t = atoi(uri + 6);
  static const uint8_t sig[8] = { 0x89, 'P', 'N', 'G', '\r', '\n', 0x1A, '\n' };
  uint8_t *o = malloc(8 + 25 + 28 + 12);
  uint8_t *p = o;
  memcpy(p, sig, 8);
  p += 8;
  be32(p, 13);
  memcpy(p + 4, "IHDR", 4);
  be32(p + 8, 1);
  be32(p + 12, 1);
  p[16] = 8; p[17] = 6; p[18] = 0; p[19] = 0; p[20] = 0;
  be32(p + 21, crc32_of(p + 4, 17));
  p += 25;
  uint8_t raw[5] = { 0, (uint8_t)(t & 255), (uint8_t)(t >> 8), PH_B, PH_A };
  uint32_t s1 = 1, s2 = 0;
  for (int i = 0; i < 5; i++) { s1 = (s1 + raw[i]) % 65521; s2 = (s2 + s1) % 65521; }
  be32(p, 16);
  memcpy(p + 4, "IDAT", 4);
  uint8_t *z = p + 8;
  z[0] = 0x78; z[1] = 0x01; z[2] = 0x01; z[3] = 5; z[4] = 0; z[5] = 0xFA; z[6] = 0xFF;
  memcpy(z + 7, raw, 5);
  be32(z + 12, s2 << 16 | s1);
  be32(p + 24, crc32_of(p + 4, 20));
  p += 28;
  be32(p, 0);
  memcpy(p + 4, "IEND", 4);
  be32(p + 8, crc32_of(p + 4, 4));
  p += 12;
  *len = (size_t)(p - o);
  return o;
}

static t3_texture **mat_slot(t3_material *m, int i) {
  switch (i) {
  case 0: return &m->map;
  case 1: return &m->normal_map;
  case 2: return &m->ao_map;
  case 3: return &m->emissive_map;
  case 4: return &m->roughness_map;
  case 5: return &m->metalness_map;
  default: return &m->alpha_map;
  }
}
static int placeholder_index(const t3_texture *t) {
  if (!t || t->width != 1 || t->height != 1 || !t->pixels || t->pixels[2] != PH_B || t->pixels[3] != PH_A) return -1;
  return t->pixels[0] | t->pixels[1] << 8;
}

typedef CW_ARRAY(t3_object *) obj_list;
static void collect(t3_object *o, void *ctx) {
  obj_list *l = ctx;
  cw_array_push(l, o);
}
static bool is_mesh(const t3_object *o) { return o->type == T3_MESH || o->type == T3_SKINNED_MESH; }

static uint64_t hash_attr(uint64_t h, const t3_attribute *a) {
  if (!a) return cw_hash_bytes("-", 1, h);
  size_t cs = a->type == T3_UINT16 ? 2 : 4;
  int32_t hdr[3] = { (int32_t)a->type, a->item_size, a->count };
  h = cw_hash_bytes(hdr, sizeof hdr, h);
  return cw_hash_bytes(a->array, (size_t)a->count * (size_t)a->item_size * cs, h);
}

static t3_geometry *lod_geometry(t3_geometry *g, const uint32_t *ix, int n) {
  t3_geometry *l = t3_geometry_new();
  for (int s = 0; s < T3_ATTR_COUNT; s++)
    if (g->attributes[s]) t3_geometry_set_attribute(l, (t3_attr_slot)s, g->attributes[s]);
  int vc = g->attributes[T3_ATTR_POSITION] ? g->attributes[T3_ATTR_POSITION]->count : 0;
  t3_attribute *ia;
  if (vc <= 65535) {
    ia = t3_attribute_new(T3_UINT16, NULL, n, 1);
    for (int i = 0; i < n; i++) ((uint16_t *)ia->array)[i] = (uint16_t)ix[i];
  } else {
    ia = t3_attribute_new(T3_UINT32, ix, n, 1);
  }
  t3_geometry_set_index(l, ia);
  t3_release(ia);
  if (g->morph_count) t3_geometry_set_morph_attributes(l, g->morph_position, g->morph_normal, g->morph_count, g->morph_relative);
  l->has_bounding_box = g->has_bounding_box;
  l->bounding_box = g->bounding_box;
  l->has_bounding_sphere = g->has_bounding_sphere;
  l->bounding_sphere = g->bounding_sphere;
  return l;
}

static int geo_triangles(const t3_geometry *g) {
  if (g->index) return g->index->count / 3;
  return g->attributes[T3_ATTR_POSITION] ? g->attributes[T3_ATTR_POSITION]->count / 3 : 0;
}

static void payload_free_internals(gltf_payload *p);

static void *gltf_finish(void *decoded, cw_res self, const cw_res *deps, int dep_count, void *u) {
  ensure_maps();
  gltf_dec *d = decoded;
  const char *path = cw_res_path(self);
  t3_gltf *g = t3_gltf_parse(d->glb, d->glb_len, placeholder_png, NULL);
  if (!g) { cw_error("res://%s: three.c's GLTFLoader rejected it", path); dec_free(d); return NULL; }
  gltf_payload *p = cw_calloc(1, sizeof *p, CW_MEM_RESOURCE);
  p->self = self;
  p->pub.gltf = g;
  p->material_n = d->material_count;
  p->materials = cw_calloc((size_t)p->material_n + 1, sizeof *p->materials, CW_MEM_RESOURCE);
  p->texture_n = d->tex_count;
  p->textures = cw_calloc((size_t)p->texture_n + 1, sizeof *p->textures, CW_MEM_RESOURCE);
  obj_list objs = { 0 };
  t3_object_traverse(g->scene, collect, &objs);
  /* textures: fill each placeholder (in texture order, so uploads are ordered) */
  int64_t tex_bytes = 0;
  for (int t = 0; t < d->tex_count; t++) {
    t3_texture *ph = NULL;
    for (int i = 0; i < objs.count && !ph; i++) {
      if (!is_mesh(objs.data[i])) continue;
      t3_mesh *m = (t3_mesh *)objs.data[i];
      for (int k = 0; k < m->material_count && !ph; k++)
        for (int s = 0; s < 7 && !ph; s++)
          if (placeholder_index(*mat_slot(m->materials[k], s)) == t) ph = *mat_slot(m->materials[k], s);
    }
    if (!ph) continue; /* no material uses it */
    int img = d->tex_image[t];
    cw_tex_payload *tp = cw_calloc(1, sizeof *tp, CW_MEM_RESOURCE);
    if (img >= 0 && img < d->image_count && d->image_ok[img] && (d->image_ext[img] < 0 || d->tex_private[t])) {
      cw_tex_cpu *cpu = &d->images[img];
      tp->info.gpu_bytes = cw__tex_upload(ph, cpu); /* same image for several textures: uploaded per texture */
      tp->info.width = cpu->width;
      tp->info.height = cpu->height;
      tp->info.levels = cpu->levels;
      tp->info.gl_internal_format = cpu->gl_format;
      tp->info.format = cpu->format;
      tp->info.from_ktx2 = cpu->from_ktx2;
      tp->tex = t3_retain(ph);
      tex_bytes += (int64_t)tp->info.gpu_bytes;
    } else if (img >= 0 && img < d->image_count && d->image_ext[img] >= 0 && d->image_ext[img] < dep_count &&
               cw_res_texture(deps[d->image_ext[img]])) {
      /* an external image: use its texture resource (shared with anyone else
       * who loads that file) in every material slot the placeholder held */
      t3_texture *real = cw_res_texture(deps[d->image_ext[img]]);
      /* (in the colour space ph wants: cw__tex_res_claim, else it went private) */
      for (int i = 0; i < objs.count; i++) {
        if (!is_mesh(objs.data[i])) continue;
        t3_mesh *m = (t3_mesh *)objs.data[i];
        for (int k = 0; k < m->material_count; k++)
          for (int s = 0; s < 7; s++)
            if (*mat_slot(m->materials[k], s) == ph) t3_material_set_texture(m->materials[k], (t3_map_slot)s, real);
      }
      cw_res_texture_info(deps[d->image_ext[img]], &tp->info);
      tp->tex = t3_retain(real);
    } else {
      cw_warn("res://%s: texture %d has no usable image; drawing it white", path, t);
      memset(ph->pixels, 255, 4);
      ph->version++;
      tp->tex = t3_retain(ph);
      tp->info.width = tp->info.height = tp->info.levels = 1;
      tp->info.format = "rgba8";
    }
    p->textures[t] = tp;
  }
  /* meshes: match three.c geometries to primitives, build LOD chains */
  int64_t geo_bytes = 0;
  uint64_t h = 0x9e3779b97f4a7c15ull;
  p->meshes = cw_calloc((size_t)d->prim_count + 1, sizeof *p->meshes, CW_MEM_RESOURCE);
  p->mesh_names = cw_calloc((size_t)d->prim_count + 1, sizeof *p->mesh_names, CW_MEM_RESOURCE);
  for (int i = 0; i < objs.count; i++) {
    if (!is_mesh(objs.data[i])) continue;
    t3_mesh *m = (t3_mesh *)objs.data[i];
    t3_geometry *geo = m->geometry;
    uint64_t v;
    if (cw_map_get(&geo_map, PKEY(geo), &v)) continue;
    t3_attribute *pos = geo->attributes[T3_ATTR_POSITION];
    if (!pos) continue;
    uint64_t key = hash_floats(pos->array, (size_t)pos->count * 3);
    int ic = geo->index ? geo->index->count : 0;
    prim_rec *r = NULL;
    for (int k = 0; k < d->prim_count && !r; k++)
      if (!d->prims[k].used && d->prims[k].key == key && d->prims[k].vcount == pos->count && d->prims[k].icount == ic) r = &d->prims[k];
    cw_mesh *cm = cw_calloc(1, sizeof *cm, CW_MEM_RESOURCE);
    cm->lod[0] = t3_retain(geo);
    cm->lod_triangles[0] = geo_triangles(geo);
    cm->lod_count = 1;
    if (geo->has_bounding_box) {
      t3_vec3 d = t3_vec3_sub(geo->bounding_box.max, geo->bounding_box.min);
      cm->extent = CW_MAX(d.x, CW_MAX(d.y, d.z));
    }
    cm->material = t3_retain(m->materials[0]);
    for (int s = 0; s < T3_ATTR_COUNT; s++) {
      if (s == T3_ATTR_INSTANCE_MATRIX || s == T3_ATTR_INSTANCE_COLOR) continue;
      h = hash_attr(h, geo->attributes[s]);
      if (geo->attributes[s]) geo_bytes += (int64_t)geo->attributes[s]->count * geo->attributes[s]->item_size * 4;
    }
    h = hash_attr(h, geo->index);
    if (geo->index) geo_bytes += (int64_t)geo->index->count * (geo->index->type == T3_UINT16 ? 2 : 4);
    p->pub.vertices += pos->count;
    p->pub.triangles += cm->lod_triangles[0];
    char nm[64];
    if (r) {
      r->used = true;
      for (int l = 1; l <= r->lod_count; l++) {
        cm->lod[l] = lod_geometry(geo, r->lod_idx[l], r->lod_n[l]);
        cm->lod_error[l] = r->lod_err[l];
        cm->lod_triangles[l] = r->lod_n[l] / 3;
        cm->lod_count++;
        h = cw_hash_bytes(r->lod_idx[l], (size_t)r->lod_n[l] * 4, h);
        geo_bytes += (int64_t)r->lod_n[l] * (pos->count <= 65535 ? 2 : 4);
      }
      if (r->material >= 0 && r->material < p->material_n && !p->materials[r->material])
        p->materials[r->material] = t3_retain(m->materials[0]);
      snprintf(nm, sizeof nm, "mesh/%d/%d", r->mesh, r->prim);
    } else {
      snprintf(nm, sizeof nm, "mesh/x%d", p->mesh_n); /* a geometry three.c made that no primitive matches */
    }
    for (int l = 0; l < cm->lod_count; l++) cw_map_set(&geo_map, PKEY(cm->lod[l]), PKEY(cm));
    if (cm->lod_count > p->pub.lod_levels) p->pub.lod_levels = cm->lod_count;
    p->meshes[p->mesh_n] = cm;
    p->mesh_names[p->mesh_n] = cw_strdup(nm, CW_MEM_RESOURCE);
    p->mesh_n++;
  }
  cw_array_free(&objs);
  p->pub.mesh_count = p->mesh_n;
  p->pub.material_count = p->material_n;
  p->pub.texture_count = p->texture_n;
  p->pub.mesh_hash = h;
  p->pub.bytes = geo_bytes + tex_bytes;
  dec_free(d);
  /* sub-resources */
  char nm[64];
  for (int i = 0; i < p->mesh_n; i++) cw_res_register_sub(self, p->mesh_names[i], CW_RES_MESH, p->meshes[i]);
  for (int i = 0; i < p->material_n; i++)
    if (p->materials[i]) { snprintf(nm, sizeof nm, "material/%d", i); cw_res_register_sub(self, nm, CW_RES_MATERIAL, p->materials[i]); }
  for (int i = 0; i < p->texture_n; i++)
    if (p->textures[i]) { snprintf(nm, sizeof nm, "texture/%d", i); cw_res_register_sub(self, nm, CW_RES_TEXTURE, p->textures[i]); }
  return p;
}

static void payload_free_internals(gltf_payload *p) {
  for (int i = 0; i < p->mesh_n; i++) {
    cw_mesh *cm = p->meshes[i];
    for (int l = 0; l < cm->lod_count; l++) {
      uint64_t v;
      if (cw_map_get(&geo_map, PKEY(cm->lod[l]), &v) && v == PKEY(cm)) cw_map_remove(&geo_map, PKEY(cm->lod[l]));
      t3_release(cm->lod[l]);
    }
    t3_release(cm->material);
    cw_free(cm);
    cw_free(p->mesh_names[i]);
  }
  cw_free(p->meshes);
  cw_free(p->mesh_names);
  for (int i = 0; i < p->material_n; i++) t3_release(p->materials[i]);
  cw_free(p->materials);
  for (int i = 0; i < p->texture_n; i++)
    if (p->textures[i]) { cw__tex_release(p->textures[i]->tex); cw_free(p->textures[i]); }
  cw_free(p->textures);
  t3_release(p->pub.gltf);
  p->meshes = NULL;
  p->mesh_names = NULL;
  p->materials = NULL;
  p->textures = NULL;
  p->pub.gltf = NULL;
  p->mesh_n = p->material_n = p->texture_n = 0;
}

static void gltf_unload(void *payload, void *u) {
  gltf_payload *p = payload;
  CW_ASSERT(!p->instances.count, "a glTF scene freed with %d live instances", p->instances.count);
  payload_free_internals(p);
  cw_array_free(&p->instances);
  cw_free(p);
}

static void gltf_discard(void *decoded, void *u) { dec_free(decoded); }

static size_t gltf_size(const void *payload, void *u) { return (size_t)((const gltf_payload *)payload)->pub.bytes; }

/* ── instances ───────────────────────────────────────────────────── */
static void copy_node(t3_object *o, const t3_object *s) {
  o->name = s->name;
  o->position = s->position;
  o->rotation = s->rotation;
  o->quaternion = s->quaternion;
  o->scale = s->scale;
  o->up = s->up;
  o->visible = s->visible;
  o->frustum_culled = s->frustum_culled;
  o->cast_shadow = s->cast_shadow;
  o->receive_shadow = s->receive_shadow;
  o->render_order = s->render_order;
  o->matrix_auto_update = s->matrix_auto_update;
  if (!s->matrix_auto_update) o->matrix = s->matrix;
  if (o->layers != s->layers) t3_object_set_layers(o, s->layers);
}

static t3_object *clone_rec(const t3_object *s, cw_map *pairs) {
  t3_object *o;
  if (s->type == T3_MESH || s->type == T3_SKINNED_MESH) {
    const t3_mesh *sm = (const t3_mesh *)s;
    t3_mesh *m;
    if (s->type == T3_SKINNED_MESH) {
      t3_skinned_mesh *k = t3_skinned_mesh_new(sm->geometry, sm->materials[0]);
      k->bind_mode = ((const t3_skinned_mesh *)s)->bind_mode;
      m = &k->mesh;
    } else {
      m = t3_mesh_new_multi(sm->geometry, sm->materials, sm->material_count);
    }
    if (sm->morph_influence_count) {
      t3_mesh_update_morph_targets(m);
      int n = CW_MIN(m->morph_influence_count, sm->morph_influence_count);
      memcpy(m->morph_influences, sm->morph_influences, (size_t)n * sizeof(float));
    }
    o = &m->base;
  } else if (s->type == T3_BONE) {
    o = t3_bone_new();
  } else if (s->type == T3_GROUP) {
    o = t3_group_new();
  } else {
    o = t3_object_new();
  }
  copy_node(o, s);
  cw_map_set(pairs, PKEY(s), PKEY(o));
  for (int i = 0; i < s->child_count; i++) {
    t3_object *c = clone_rec(s->children[i], pairs);
    t3_object_add(o, c);
    t3_release(c);
  }
  return o;
}

/* skinned meshes: a skeleton over the instance's own bones */
static void bind_skins(const t3_object *s, cw_map *pairs) {
  if (s->type == T3_SKINNED_MESH) {
    const t3_skinned_mesh *ss = (const t3_skinned_mesh *)s;
    uint64_t v;
    if (ss->skeleton && cw_map_get(pairs, PKEY(s), &v)) {
      t3_skinned_mesh *ds = (t3_skinned_mesh *)(uintptr_t)v;
      int n = ss->skeleton->bone_count;
      t3_object **bones = cw_alloc((size_t)(n ? n : 1) * sizeof *bones, CW_MEM_RESOURCE);
      for (int i = 0; i < n; i++) {
        uint64_t b;
        bones[i] = cw_map_get(pairs, PKEY(ss->skeleton->bones[i]), &b) ? (t3_object *)(uintptr_t)b : ss->skeleton->bones[i];
      }
      t3_skeleton *sk = t3_skeleton_new(bones, n, ss->skeleton->bone_inverses);
      t3_skinned_mesh_bind(ds, sk, &ss->bind_matrix);
      t3_release(sk);
      cw_free(bones);
    }
  }
  for (int i = 0; i < s->child_count; i++) bind_skins(s->children[i], pairs);
}

static void fill_instance(t3_object *root, const t3_object *tmpl) {
  cw_map pairs;
  cw_map_init(&pairs, CW_MEM_RESOURCE);
  root->name = tmpl->name;
  for (int i = 0; i < tmpl->child_count; i++) {
    t3_object *c = clone_rec(tmpl->children[i], &pairs);
    t3_object_add(root, c);
    t3_release(c);
  }
  bind_skins(tmpl, &pairs);
  cw_map_free(&pairs);
}

static gltf_payload *payload_of_instance(t3_object *root, cw_res *res) {
  uint64_t v;
  if (!maps_init || !cw_map_get(&inst_map, PKEY(root), &v)) return NULL;
  *res = (cw_res){ v };
  return cw__res_get_typed(*res, CW_RES_GLTF);
}

static void instance_destroyed(t3_object *root) {
  cw_res r = { 0 };
  gltf_payload *p = payload_of_instance(root, &r);
  if (p)
    for (int i = 0; i < p->instances.count; i++)
      if (p->instances.data[i] == root) {
        memmove(p->instances.data + i, p->instances.data + i + 1, (size_t)(p->instances.count - i - 1) * sizeof *p->instances.data);
        p->instances.count--;
        break;
      }
  if (maps_init) {
    cw_map_remove(&inst_map, PKEY(root));
    cw_map_remove(&inst_lod, PKEY(root));
  }
  cw_res_release(r);
}

t3_object *cw_gltf_instantiate(cw_res scene) {
  gltf_payload *p = cw__res_get_typed(scene, CW_RES_GLTF);
  if (!p) return NULL;
  ensure_maps();
  t3_object *root = t3_group_new();
  fill_instance(root, p->pub.gltf->scene);
  root->on_destroy = instance_destroyed;
  cw_map_set(&inst_map, PKEY(root), scene.id);
  cw_array_push(&p->instances, root);
  cw_res_add_ref(scene);
  return root;
}

int cw_gltf_instance_count(cw_res scene) {
  gltf_payload *p = cw__res_get_typed(scene, CW_RES_GLTF);
  return p ? p->instances.count : 0;
}

typedef struct { int level, tris; } lod_ctx;
static void set_lod(t3_object *o, void *ctx) {
  lod_ctx *c = ctx;
  if (!is_mesh(o)) return;
  t3_mesh *m = (t3_mesh *)o;
  uint64_t v;
  if (!cw_map_get(&geo_map, PKEY(m->geometry), &v)) { c->tris += geo_triangles(m->geometry); return; }
  cw_mesh *cm = (cw_mesh *)(uintptr_t)v;
  int l = CW_CLAMP(c->level, 0, cm->lod_count - 1);
  if (m->geometry != cm->lod[l]) {
    t3_retain(cm->lod[l]);
    t3_release(m->geometry);
    m->geometry = cm->lod[l];
  }
  c->tris += cm->lod_triangles[l];
}

int cw_gltf_instance_set_lod(t3_object *instance, int level) {
  if (!instance) return 0;
  ensure_maps();
  lod_ctx c = { level < 0 ? 0 : level, 0 };
  t3_object_traverse(instance, set_lod, &c);
  if (cw_map_get(&inst_map, PKEY(instance), &(uint64_t){ 0 })) cw_map_set(&inst_lod, PKEY(instance), (uint64_t)c.level);
  return c.tris;
}

typedef struct { t3_vec3 eye; float k, max_px; bool ortho; int tris; } auto_ctx;
static void auto_lod(t3_object *o, void *ctx) {
  auto_ctx *c = ctx;
  if (!is_mesh(o)) return;
  t3_mesh *m = (t3_mesh *)o;
  uint64_t v;
  if (!cw_map_get(&geo_map, PKEY(m->geometry), &v)) { c->tris += geo_triangles(m->geometry); return; }
  cw_mesh *cm = (cw_mesh *)(uintptr_t)v;
  t3_geometry *g0 = cm->lod[0];
  float scale = t3_mat4_max_scale_on_axis(&o->matrix_world);
  float d = 1;
  if (!c->ortho) {
    t3_vec3 ctr = g0->has_bounding_sphere ? t3_vec3_apply_mat4(g0->bounding_sphere.center, &o->matrix_world)
                                          : t3_mat4_get_position(&o->matrix_world);
    float r = g0->has_bounding_sphere ? g0->bounding_sphere.radius * scale : 0;
    d = t3_vec3_distance(ctr, c->eye) - r; /* the nearest the mesh gets */
    if (d < 1e-3f) d = 1e-3f;
  }
  int pick = 0;
  for (int l = 1; l < cm->lod_count; l++)
    if (cm->lod_error[l] * cm->extent * scale / d * c->k <= c->max_px) pick = l;
  if (m->geometry != cm->lod[pick]) {
    t3_retain(cm->lod[pick]);
    t3_release(m->geometry);
    m->geometry = cm->lod[pick];
  }
  c->tris += cm->lod_triangles[pick];
}

int cw_gltf_instance_auto_lod(t3_object *instance, const t3_camera *camera, float viewport_height, float max_error_px) {
  if (!instance || !camera) return 0;
  ensure_maps();
  t3_object_update_world_matrix(instance, true, true);
  t3_object_update_world_matrix((void *)camera, true, false);
  auto_ctx c = { t3_mat4_get_position(&camera->base.matrix_world), 0, max_error_px,
                 camera->base.type == T3_ORTHOGRAPHIC_CAMERA, 0 };
  /* pixels per unit at distance 1: projection y scale x half the viewport
   * (orthographic: the same, with no distance) */
  c.k = camera->projection_matrix.e[5] * viewport_height * 0.5f;
  t3_object_traverse(instance, auto_lod, &c);
  cw_map_remove(&inst_lod, PKEY(instance)); /* auto, not a fixed level to restore after a reload */
  return c.tris;
}

static int coarsest_lod(const t3_object *o) {
  int best = 0;
  if (is_mesh(o)) {
    uint64_t v;
    const t3_mesh *m = (const t3_mesh *)o;
    if (cw_map_get(&geo_map, PKEY(m->geometry), &v)) {
      const cw_mesh *cm = (const cw_mesh *)(uintptr_t)v;
      for (int l = 0; l < cm->lod_count; l++)
        if (cm->lod[l] == m->geometry) best = l;
    }
  }
  for (int i = 0; i < o->child_count; i++) best = CW_MAX(best, coarsest_lod(o->children[i]));
  return best;
}
int cw_gltf_instance_lod(const t3_object *instance) { return instance && maps_init ? coarsest_lod(instance) : 0; }

int cw_gltf_auto_lod_all(const t3_camera *camera, float viewport_height, float max_error_px) {
  if (!maps_init) return 0;
  int tris = 0;
  for (int i = 0; i < inst_map.count; i++)
    if (cw_map_live(&inst_map, i))
      tris += cw_gltf_instance_auto_lod((t3_object *)(uintptr_t)inst_map.keys[i], camera, viewport_height, max_error_px);
  return tris;
}

/* hot reload: the payload keeps its address (sub-resources and instance
 * lists point at it); swap in fresh's internals, rebuild every instance */
static void gltf_reload(void *payload, void *fresh, void *u) {
  gltf_payload *p = payload, *f = fresh;
  gltf_payload tmp = *p;
  cw_res self = p->self;
  *p = *f;
  p->self = self;
  p->instances = tmp.instances;
  *f = tmp;
  f->instances.data = NULL;
  f->instances.count = f->instances.cap = 0;
  for (int i = 0; i < p->instances.count; i++) {
    t3_object *root = p->instances.data[i];
    while (root->child_count) t3_object_remove(root, root->children[root->child_count - 1]);
    fill_instance(root, p->pub.gltf->scene);
    uint64_t lv;
    if (cw_map_get(&inst_lod, PKEY(root), &lv)) cw_gltf_instance_set_lod(root, (int)lv);
  }
  gltf_unload(f, u);
}

const cw_res_type_desc cw__res_gltf_type = {
  .name = "gltf", .extensions = ".glb .gltf", .prepare = gltf_prepare, .decode = gltf_decode,
  .finish = gltf_finish, .unload = gltf_unload, .discard = gltf_discard, .reload = gltf_reload, .size = gltf_size,
};

const cw_gltf *cw_res_gltf(cw_res h) {
  gltf_payload *p = cw__res_get_typed(h, CW_RES_GLTF);
  return p ? &p->pub : NULL;
}

/* ── mesh / material: only ever sub-resources of a glTF ──────────── */
static void *sub_finish(void *decoded, cw_res self, const cw_res *deps, int n, void *u) {
  cw_error("res://%s: meshes and materials come from glTF files: load \"file.glb#mesh/0/0\" or \"#material/0\"",
           cw_res_path(self));
  return NULL;
}
static void sub_unload(void *payload, void *u) {}
static size_t mesh_size(const void *payload, void *u) {
  const cw_mesh *m = payload;
  size_t b = 0;
  for (int l = 1; l < m->lod_count; l++) b += (size_t)m->lod_triangles[l] * 3 * 4;
  return b;
}
const cw_res_type_desc cw__res_mesh_type = { .name = "mesh", .finish = sub_finish, .unload = sub_unload, .size = mesh_size };
const cw_res_type_desc cw__res_material_type = { .name = "material", .finish = sub_finish, .unload = sub_unload };

const cw_mesh *cw_res_mesh(cw_res h) { return cw__res_get_typed(h, CW_RES_MESH); }
t3_material *cw_res_material(cw_res h) { return cw__res_get_typed(h, CW_RES_MATERIAL); }

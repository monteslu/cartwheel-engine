#include "gltf_doc.h"

#include <stdio.h>
#include <stdlib.h>

#include "meshoptimizer.h"

#define ERR(...) (snprintf(err, err_cap, __VA_ARGS__), false)

static uint32_t rd32(const uint8_t *p) { return p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24; }
static void wr32(uint8_t *p, uint32_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24); }

bool cw_glb_split(const void *data, size_t len, const char **json, size_t *json_len, const uint8_t **bin,
                  size_t *bin_len) {
  const uint8_t *p = data;
  *bin = NULL;
  *bin_len = 0;
  if (len >= 20 && !memcmp(p, "glTF", 4)) {
    if (rd32(p + 4) != 2) return false;
    size_t total = rd32(p + 8);
    if (total > len) return false;
    size_t off = 12;
    *json = NULL;
    while (off + 8 <= total) {
      size_t n = rd32(p + off);
      uint32_t type = rd32(p + off + 4);
      if (off + 8 + n > total) return false;
      if (type == 0x4E4F534A && !*json) { *json = (const char *)p + off + 8; *json_len = n; }
      else if (type == 0x004E4942 && !*bin) { *bin = p + off + 8; *bin_len = n; }
      off += 8 + ((n + 3) & ~(size_t)3);
    }
    return *json != NULL;
  }
  size_t i = 0;
  while (i < len && (p[i] == ' ' || p[i] == '\n' || p[i] == '\r' || p[i] == '\t' || p[i] == 0xEF || p[i] == 0xBB || p[i] == 0xBF)) i++;
  if (i < len && p[i] == '{') { *json = data; *json_len = len; return true; }
  return false;
}

uint8_t *cw_glb_build(const char *json, size_t json_len, const uint8_t *bin, size_t bin_len, size_t *out_len) {
  size_t jp = (json_len + 3) & ~(size_t)3, bp = (bin_len + 3) & ~(size_t)3;
  size_t total = 12 + 8 + jp + (bin ? 8 + bp : 0);
  uint8_t *o = cw_alloc(total, CW_MEM_RESOURCE);
  memcpy(o, "glTF", 4);
  wr32(o + 4, 2);
  wr32(o + 8, (uint32_t)total);
  wr32(o + 12, (uint32_t)jp);
  wr32(o + 16, 0x4E4F534A);
  memcpy(o + 20, json, json_len);
  memset(o + 20 + json_len, ' ', jp - json_len);
  if (bin) {
    uint8_t *b = o + 20 + jp;
    wr32(b, (uint32_t)bp);
    wr32(b + 4, 0x004E4942);
    if (bin_len) memcpy(b + 8, bin, bin_len);
    memset(b + 8 + bin_len, 0, bp - bin_len);
  }
  *out_len = total;
  return o;
}

static int b64v(char c) {
  if (c >= 'A' && c <= 'Z') return c - 'A';
  if (c >= 'a' && c <= 'z') return c - 'a' + 26;
  if (c >= '0' && c <= '9') return c - '0' + 52;
  if (c == '+' || c == '-') return 62;
  if (c == '/' || c == '_') return 63;
  return -1;
}
static uint8_t *b64_decode(const char *s, size_t *out_len) {
  size_t n = strlen(s);
  uint8_t *o = cw_alloc(n * 3 / 4 + 3, CW_MEM_RESOURCE);
  size_t k = 0;
  uint32_t acc = 0;
  int bits = 0;
  for (size_t i = 0; i < n; i++) {
    int v = b64v(s[i]);
    if (v < 0) continue;
    acc = acc << 6 | (uint32_t)v;
    bits += 6;
    if (bits >= 8) { bits -= 8; o[k++] = (uint8_t)(acc >> bits); }
  }
  *out_len = k;
  return o;
}

/* uri percent-decoding ("my%20file.bin") */
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

static cwj *ensure_arr(cwj *root, const char *key) {
  cwj *a = cwj_get(root, key);
  if (!a || a->type != CWJ_ARR) { a = cwj_arr(); cwj_set(root, key, a); }
  return a;
}

/* buffer data, cached in the doc's owned arrays: index i of the buffers array */
typedef struct { const uint8_t *data; size_t len; } bufref;

static bool resolve_views(cw_gltf_doc *d, const bufref *bufs, int nb, char *err, size_t err_cap) {
  cwj *views = cwj_get(d->root, "bufferViews");
  int n = views && views->type == CWJ_ARR ? views->count : 0;
  d->view_cap = n + 16;
  d->views = cw_calloc((size_t)d->view_cap, sizeof *d->views, CW_MEM_RESOURCE);
  d->view_count = n;
  for (int i = 0; i < n; i++) {
    cwj *v = views->items[i];
    int b = (int)cwj_int(v, "buffer", -1);
    size_t off = (size_t)cwj_int(v, "byteOffset", 0), len = (size_t)cwj_int(v, "byteLength", 0);
    if (b < 0 || b >= nb) return ERR("bufferView %d: bad buffer %d", i, b);
    d->views[i].len = len;
    if (bufs[b].data) {
      if (off + len > bufs[b].len) return ERR("bufferView %d: range %zu+%zu outside buffer %d (%zu bytes)", i, off, len, b, bufs[b].len);
      d->views[i].data = bufs[b].data + off;
    }
  }
  return true;
}

bool cw_gltf_doc_open(cw_gltf_doc *d, const void *file, size_t len, cw_gltf_read_fn read, void *ctx, char *err,
                      size_t err_cap) {
  memset(d, 0, sizeof *d);
  const char *json;
  size_t json_len, bin_len;
  const uint8_t *bin;
  if (!cw_glb_split(file, len, &json, &json_len, &bin, &bin_len)) return ERR("not a glTF 2.0 file (GLB or JSON)");
  d->root = cwj_parse(json, json_len, err, err_cap);
  if (!d->root) return false;
  if (d->root->type != CWJ_OBJ) return ERR("glTF JSON is not an object");
  cwj *buffers = cwj_get(d->root, "buffers");
  int nb = buffers && buffers->type == CWJ_ARR ? buffers->count : 0;
  bufref *bufs = cw_calloc((size_t)(nb ? nb : 1), sizeof *bufs, CW_MEM_RESOURCE);
  d->owned_buffers = cw_calloc((size_t)(nb ? nb : 1), sizeof *d->owned_buffers, CW_MEM_RESOURCE);
  bool ok = true;
  for (int i = 0; i < nb && ok; i++) {
    cwj *b = buffers->items[i];
    const char *uri = cwj_string(b, "uri");
    size_t want = (size_t)cwj_int(b, "byteLength", 0);
    if (!uri) {
      cwj *ext = cwj_get(cwj_get(b, "extensions"), "EXT_meshopt_compression");
      if (ext && cwj_get(ext, "fallback")) continue; /* no data by design */
      if (i == 0 && bin) { bufs[i].data = bin; bufs[i].len = bin_len; }
      else ok = ERR("buffer %d has no uri and the file has no GLB binary chunk", i);
    } else if (!strncmp(uri, "data:", 5)) {
      const char *comma = strchr(uri, ',');
      if (!comma) { ok = ERR("buffer %d: malformed data uri", i); break; }
      size_t n;
      d->owned_buffers[d->owned_count++] = (uint8_t *)(bufs[i].data = b64_decode(comma + 1, &n));
      bufs[i].len = n;
    } else {
      char path[1024];
      uri_decode(uri, path, sizeof path);
      size_t n = 0;
      void *bytes = read ? read(ctx, path, &n) : NULL;
      if (!bytes) { ok = ERR("buffer %d: cannot read \"%s\" (beside the .gltf)", i, path); break; }
      d->owned_buffers[d->owned_count++] = bytes;
      bufs[i].data = bytes;
      bufs[i].len = n;
    }
    if (ok && bufs[i].data && bufs[i].len < want) ok = ERR("buffer %d: %zu bytes, byteLength says %zu", i, bufs[i].len, want);
  }
  if (ok) ok = resolve_views(d, bufs, nb, err, err_cap);
  /* keep buffer refs for meshopt decoding: stash in the extension objects' order */
  if (ok) {
    /* compressed sources live in buffers; record them per view now */
    cwj *views = cwj_get(d->root, "bufferViews");
    for (int i = 0; i < d->view_count; i++) {
      cwj *ext = cwj_get(cwj_get(views->items[i], "extensions"), "EXT_meshopt_compression");
      if (!ext) continue;
      int b = (int)cwj_int(ext, "buffer", -1);
      size_t off = (size_t)cwj_int(ext, "byteOffset", 0), n = (size_t)cwj_int(ext, "byteLength", 0);
      if (b < 0 || b >= nb || !bufs[b].data || off + n > bufs[b].len) { ok = ERR("bufferView %d: bad EXT_meshopt_compression source", i); break; }
      /* remember the compressed source in the (otherwise empty) data slot of a fallback view */
      if (!d->views[i].data) { d->views[i].data = bufs[b].data + off; d->views[i].len = n; d->views[i].compress = -1; }
      else { ok = ERR("bufferView %d: compressed view must live in a fallback buffer", i); break; }
    }
  }
  cw_free(bufs);
  if (!ok) cw_gltf_doc_free(d);
  return ok;
}

void cw_gltf_doc_free(cw_gltf_doc *d) {
  for (int i = 0; i < d->view_count; i++) cw_free(d->views[i].owned);
  cw_free(d->views);
  for (int i = 0; i < d->owned_count; i++) cw_free(d->owned_buffers[i]);
  cw_free(d->owned_buffers);
  cwj_free(d->root);
  memset(d, 0, sizeof *d);
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

bool cw_gltf_doc_meshopt_decode(cw_gltf_doc *d, char *err, size_t err_cap) {
  cwj *views = cwj_get(d->root, "bufferViews");
  for (int i = 0; i < d->view_count; i++) {
    cwj *exts = cwj_get(views->items[i], "extensions");
    cwj *ext = cwj_get(exts, "EXT_meshopt_compression");
    if (!ext) continue;
    size_t count = (size_t)cwj_int(ext, "count", 0), stride = (size_t)cwj_int(ext, "byteStride", 0);
    const char *mode = cwj_string(ext, "mode"), *filter = cwj_string(ext, "filter");
    size_t out_len = (size_t)cwj_int(views->items[i], "byteLength", 0);
    if (count * stride > out_len) return ERR("bufferView %d: meshopt count x stride exceeds byteLength", i);
    uint8_t *dst = cw_calloc(1, out_len ? out_len : 1, CW_MEM_RESOURCE);
    const uint8_t *src = d->views[i].data;
    size_t src_len = d->views[i].len;
    int rc = -1;
    if (mode && !strcmp(mode, "ATTRIBUTES")) {
      rc = meshopt_decodeVertexBuffer(dst, count, stride, src, src_len);
      if (rc == 0 && filter) {
        if (!strcmp(filter, "OCTAHEDRAL")) meshopt_decodeFilterOct(dst, count, stride);
        else if (!strcmp(filter, "QUATERNION")) meshopt_decodeFilterQuat(dst, count, stride);
        else if (!strcmp(filter, "EXPONENTIAL")) meshopt_decodeFilterExp(dst, count, stride);
        else if (!strcmp(filter, "COLOR")) meshopt_decodeFilterColor(dst, count, stride);
      }
    } else if (mode && !strcmp(mode, "TRIANGLES")) {
      rc = meshopt_decodeIndexBuffer(dst, count, stride, src, src_len);
    } else if (mode && !strcmp(mode, "INDICES")) {
      rc = meshopt_decodeIndexSequence(dst, count, stride, src, src_len);
    }
    if (rc != 0) { cw_free(dst); return ERR("bufferView %d: meshopt decode failed (mode %s, code %d)", i, mode ? mode : "?", rc); }
    d->views[i].data = dst;
    d->views[i].owned = dst;
    d->views[i].len = out_len;
    d->views[i].compress = CW_MESHOPT_NONE;
    cwj_remove(exts, "EXT_meshopt_compression");
    if (!exts->count) cwj_remove(views->items[i], "extensions");
  }
  drop_ext_name(d->root, "extensionsUsed", "EXT_meshopt_compression");
  drop_ext_name(d->root, "extensionsRequired", "EXT_meshopt_compression");
  return true;
}

int cw_gltf_doc_add_view(cw_gltf_doc *d, uint8_t *owned, size_t len, int stride, int target) {
  cwj *views = ensure_arr(d->root, "bufferViews");
  CW_ASSERT(views->count == d->view_count, "bufferViews edited outside cw_gltf_doc_add_view");
  cwj *v = cwj_obj();
  cwj_set(v, "buffer", cwj_num(0));
  cwj_set(v, "byteOffset", cwj_num(0));
  cwj_set(v, "byteLength", cwj_num((double)len));
  if (stride) cwj_set(v, "byteStride", cwj_num(stride));
  if (target) cwj_set(v, "target", cwj_num(target));
  cwj_push(views, v);
  if (d->view_count == d->view_cap) {
    d->view_cap = d->view_cap ? d->view_cap * 2 : 16;
    d->views = cw_realloc(d->views, (size_t)d->view_cap * sizeof *d->views, CW_MEM_RESOURCE);
  }
  cw_gltf_view *gv = &d->views[d->view_count];
  memset(gv, 0, sizeof *gv);
  gv->data = gv->owned = owned;
  gv->len = len;
  return d->view_count++;
}

int cw_gltf_accessor_elem_size(const cwj *a) {
  int ct = (int)cwj_int(a, "componentType", 0);
  int cs = ct == 5120 || ct == 5121 ? 1 : ct == 5122 || ct == 5123 ? 2 : ct == 5125 || ct == 5126 ? 4 : 0;
  const char *t = cwj_string(a, "type");
  int nc = !t ? 0 : !strcmp(t, "SCALAR") ? 1 : !strcmp(t, "VEC2") ? 2 : !strcmp(t, "VEC3") ? 3 : !strcmp(t, "VEC4") ? 4
           : !strcmp(t, "MAT2") ? 4 : !strcmp(t, "MAT3") ? 9 : !strcmp(t, "MAT4") ? 16 : 0;
  return cs * nc;
}

/* ── writing ─────────────────────────────────────────────────────── */
typedef struct { uint8_t *d; size_t n, cap; } bytes;
static size_t put(bytes *b, const void *src, size_t n, size_t align) {
  size_t at = (b->n + align - 1) & ~(align - 1);
  size_t need = at + n;
  if (need > b->cap) {
    while (need > b->cap) b->cap = b->cap ? b->cap * 2 : 65536;
    b->d = cw_realloc(b->d, b->cap, CW_MEM_RESOURCE);
  }
  memset(b->d + b->n, 0, at - b->n);
  if (n) memcpy(b->d + at, src, n);
  b->n = need;
  return at;
}

static void remap_ref(cwj *o, const char *key, const int *map, int n) {
  cwj *v = cwj_get(o, key);
  if (!v || v->type != CWJ_NUM) return;
  int i = (int)v->num;
  if (i >= 0 && i < n && map[i] >= 0) cwj_set(o, key, cwj_num(map[i]));
}

uint8_t *cw_gltf_doc_write_glb(cw_gltf_doc *d, size_t *out_len) {
  cwj *root = d->root;
  cwj *views = ensure_arr(root, "bufferViews");
  int n = d->view_count;
  bool *used = cw_calloc((size_t)(n ? n : 1), 1, CW_MEM_RESOURCE);
  int *map = cw_alloc((size_t)(n ? n : 1) * sizeof *map, CW_MEM_RESOURCE);
  cwj *accessors = cwj_get(root, "accessors"), *images = cwj_get(root, "images");
#define MARK(o, k) do { int _i = (int)cwj_int((o), (k), -1); if (_i >= 0 && _i < n) used[_i] = true; } while (0)
  for (int i = 0; accessors && i < accessors->count; i++) {
    cwj *a = accessors->items[i];
    MARK(a, "bufferView");
    cwj *sp = cwj_get(a, "sparse");
    if (sp) { MARK(cwj_get(sp, "indices"), "bufferView"); MARK(cwj_get(sp, "values"), "bufferView"); }
  }
  for (int i = 0; images && i < images->count; i++) MARK(images->items[i], "bufferView");
#undef MARK
  bool any_compress = false;
  int m = 0;
  for (int i = 0; i < n; i++) {
    map[i] = used[i] ? m++ : -1;
    if (used[i] && d->views[i].compress > 0) any_compress = true;
  }
  size_t align = any_compress ? 16 : 4;
  bytes bin = { 0 };
  size_t fallback = 0;
  cwj *nv = cwj_arr();
  for (int i = 0; i < n; i++) {
    if (!used[i]) continue;
    cw_gltf_view *v = &d->views[i];
    CW_ASSERT(v->data || !v->len, "bufferView %d has no data: decode meshopt views before writing", i);
    cwj *o = cwj_clone(views->items[i]);
    cwj *exts = cwj_get(o, "extensions");
    if (exts) { cwj_remove(exts, "EXT_meshopt_compression"); if (!exts->count) cwj_remove(o, "extensions"); }
    if (v->compress > 0) {
      size_t cap = v->compress == CW_MESHOPT_ATTRIBUTES ? meshopt_encodeVertexBufferBound((size_t)v->count, (size_t)v->stride)
                                                         : meshopt_encodeIndexBufferBound((size_t)v->count, (size_t)v->count);
      uint8_t *enc = cw_alloc(cap, CW_MEM_RESOURCE);
      size_t en;
      if (v->compress == CW_MESHOPT_ATTRIBUTES) {
        en = meshopt_encodeVertexBufferLevel(enc, cap, v->data, (size_t)v->count, (size_t)v->stride, 2, 0);
      } else {
        /* index codec input is u32 */
        uint32_t *ix = cw_alloc((size_t)v->count * 4, CW_MEM_RESOURCE);
        for (int k = 0; k < v->count; k++)
          ix[k] = v->stride == 2 ? ((const uint16_t *)v->data)[k] : ((const uint32_t *)v->data)[k];
        en = v->compress == CW_MESHOPT_TRIANGLES ? meshopt_encodeIndexBuffer(enc, cap, ix, (size_t)v->count)
                                                 : meshopt_encodeIndexSequence(enc, cap, ix, (size_t)v->count);
        cw_free(ix);
      }
      size_t at = put(&bin, enc, en, 4);
      cw_free(enc);
      fallback = (fallback + 3) & ~(size_t)3;
      cwj_set(o, "buffer", cwj_num(1));
      cwj_set(o, "byteOffset", cwj_num((double)fallback));
      cwj_set(o, "byteLength", cwj_num((double)v->len));
      if (v->compress == CW_MESHOPT_ATTRIBUTES) cwj_set(o, "byteStride", cwj_num(v->stride));
      fallback += v->len;
      cwj *e = cwj_obj();
      cwj_set(e, "buffer", cwj_num(0));
      cwj_set(e, "byteOffset", cwj_num((double)at));
      cwj_set(e, "byteLength", cwj_num((double)en));
      cwj_set(e, "byteStride", cwj_num(v->stride));
      cwj_set(e, "mode", cwj_str(v->compress == CW_MESHOPT_ATTRIBUTES ? "ATTRIBUTES" : v->compress == CW_MESHOPT_TRIANGLES ? "TRIANGLES" : "INDICES"));
      cwj_set(e, "count", cwj_num(v->count));
      if (!(exts = cwj_get(o, "extensions"))) { exts = cwj_obj(); cwj_set(o, "extensions", exts); }
      cwj_set(exts, "EXT_meshopt_compression", e);
    } else {
      size_t at = put(&bin, v->data, v->len, align);
      cwj_set(o, "buffer", cwj_num(0));
      cwj_set(o, "byteOffset", cwj_num((double)at));
      cwj_set(o, "byteLength", cwj_num((double)v->len));
    }
    cwj_push(nv, o);
  }
  /* renumber references */
  for (int i = 0; accessors && i < accessors->count; i++) {
    cwj *a = accessors->items[i];
    remap_ref(a, "bufferView", map, n);
    cwj *sp = cwj_get(a, "sparse");
    if (sp) { remap_ref(cwj_get(sp, "indices"), "bufferView", map, n); remap_ref(cwj_get(sp, "values"), "bufferView", map, n); }
  }
  for (int i = 0; images && i < images->count; i++) remap_ref(images->items[i], "bufferView", map, n);
  /* the document now matches the written form: views renumbered, one buffer */
  cw_gltf_view *kept = cw_calloc((size_t)(m ? m : 1) + 16, sizeof *kept, CW_MEM_RESOURCE);
  for (int i = 0; i < n; i++) {
    if (map[i] >= 0) kept[map[i]] = d->views[i];
    else cw_free(d->views[i].owned);
  }
  cw_free(d->views);
  d->views = kept;
  d->view_count = m;
  d->view_cap = m + 16;
  if (m) cwj_set(root, "bufferViews", nv);
  else { cwj_free(nv); cwj_remove(root, "bufferViews"); }
  cwj *bufs = cwj_arr();
  if (bin.n || m) {
    cwj *b0 = cwj_obj();
    cwj_set(b0, "byteLength", cwj_num((double)bin.n));
    cwj_push(bufs, b0);
  }
  if (any_compress) {
    cwj *b1 = cwj_obj(), *e = cwj_obj(), *f = cwj_obj();
    cwj_set(b1, "byteLength", cwj_num((double)fallback));
    cwj_set(f, "fallback", cwj_bool(true));
    cwj_set(e, "EXT_meshopt_compression", f);
    cwj_set(b1, "extensions", e);
    cwj_push(bufs, b1);
    for (int pass = 0; pass < 2; pass++) {
      cwj *list = ensure_arr(root, pass ? "extensionsRequired" : "extensionsUsed");
      bool has = false;
      for (int i = 0; i < list->count; i++) has |= list->items[i]->type == CWJ_STR && !strcmp(list->items[i]->text, "EXT_meshopt_compression");
      if (!has) cwj_push(list, cwj_str("EXT_meshopt_compression"));
    }
  }
  if (bufs->count) cwj_set(root, "buffers", bufs);
  else { cwj_free(bufs); cwj_remove(root, "buffers"); }
  size_t jl;
  char *js = cwj_write(root, &jl);
  uint8_t *out = cw_glb_build(js, jl, bin.n || m ? (bin.d ? bin.d : (const uint8_t *)"") : NULL, bin.n, out_len);
  cw_free(js);
  /* the views now point at the new bin's layout only through the JSON; keep
   * their bytes (owned or borrowed) for any later edit */
  cw_free(bin.d);
  cw_free(used);
  cw_free(map);
  return out;
}

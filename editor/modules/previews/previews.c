/* Resource previews (M4.2): thumbnails of models, glTF meshes and
 * materials, textures and scenes, rendered off-screen through the engine's
 * own pipeline into a render target (pipeline.h cw_rt), read back and kept
 * as UI textures. Cached by CONTENT: the key hashes the file's bytes, the
 * files it reads (a .gltf's buffers and images) and the preview version, so
 * a renamed file keeps its thumbnail and a changed one gets a new one.
 * Renders run one per frame in request order (the same frames on every host).
 *
 * Also the Preview panel: the active scene drawn through its own camera
 * (re-rendered when it changes); dropping a file there places it on the
 * ground under the pointer (fs.place with that position).
 *
 *   preview.get <path>        thumbnail state (queues a render): hash, ready,
 *                             texture (a cw_texture id, editor process only),
 *                             coverage (share of non-background pixels)
 *   preview.render <path>     render it now (no queue, no cache)
 *   preview.scene             the active scene rendered now: non-background
 *                             pixels and an image hash
 *   preview.node_pixels <n>   how many pixels node n covers in that render
 *                             (rendered again with n hidden; nothing saved)
 * Sub-resources of a glTF preview too: "file.glb#mesh/0/0", "#material/1". */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cartwheel/debug.h"
#include "cartwheel/gl.h"
#include "cartwheel/pipeline.h"
#include "cartwheel/render2d.h"
#include "cartwheel/render3d.h"
#include "cartwheel/resource.h"
#include "cartwheel/vfs.h"
#include "cwed/editor.h"
#include "cwpack.h"

#define THUMB_PX 128
#define MAX_CACHED 256 /* thumbnails kept (128x128 RGBA each in the UI atlas) */
#define PREVIEW_VERSION 1u
static const float BG[3] = { 0.15f, 0.16f, 0.19f };

/* ── cache ─────────────────────────────────────────────────────────────── */

typedef struct pv {
  uint64_t hash;
  uint64_t last_use; /* request counter: the least recently asked go first past MAX_CACHED */
  cw_texture tex;
  bool ready;
  float coverage;
  char error[1200];
} pv;
static CW_ARRAY(pv) cache;

typedef struct { const char *path; uint64_t hash; uint32_t gen; } path_key;
static CW_ARRAY(path_key) keys;
static CW_ARRAY(const char *) queue; /* paths waiting for a render */
static int renders, f_renders, f_queued;

static const char *ext_lower(const char *p) {
  static char e[16];
  const char *hash = strchr(p, '#');
  char base[512];
  snprintf(base, sizeof base, "%.*s", hash ? (int)(hash - p) : (int)strlen(p), p);
  const char *dot = strrchr(base, '.'), *slash = strrchr(base, '/');
  e[0] = 0;
  if (!dot || (slash && dot < slash) || strlen(dot) >= sizeof e) return e;
  for (size_t i = 0; i <= strlen(dot); i++) e[i] = (char)(dot[i] >= 'A' && dot[i] <= 'Z' ? dot[i] + 32 : dot[i]);
  return e;
}
static bool ext_in(const char *p, const char *list) {
  const char *e = ext_lower(p);
  size_t n = strlen(e);
  if (!n) return false;
  for (const char *s = list; (s = strstr(s, e)); s += n)
    if ((s == list || s[-1] == ' ') && (s[n] == 0 || s[n] == ' ')) return true;
  return false;
}
/* model, mesh, material, texture, scene; NULL = no preview */
static const char *kind_of(const char *p) {
  const char *h = strchr(p, '#');
  if (h) return !strncmp(h + 1, "mesh/", 5) ? "mesh" : !strncmp(h + 1, "material/", 9) ? "material" : NULL;
  if (ext_in(p, ".gltf .glb")) return "model";
  if (ext_in(p, ".png .jpg .jpeg .ktx2")) return "texture";
  if (ext_in(p, ".cwscene")) return "scene";
  return NULL;
}

typedef struct { const char *base; uint64_t key; } dep_ctx;
static void hash_dep(const char *uri, void *user) {
  dep_ctx *d = user;
  char dec[512], full[1024];
  cwp_uri_decode(uri, dec, sizeof dec);
  const char *slash = strrchr(d->base, '/');
  snprintf(full, sizeof full, "%.*s%s", slash ? (int)(slash - d->base) + 1 : 0, d->base, dec);
  size_t n;
  void *b = cw_vfs_read(full, &n);
  d->key = cw_hash_bytes(full, strlen(full), d->key);
  if (b) d->key = cw_hash_bytes(b, n, d->key);
  free(b);
}

/* content hash of what a preview shows (0 when the file is missing) */
static uint64_t content_hash(const char *path) {
  char file[512];
  const char *h = strchr(path, '#');
  snprintf(file, sizeof file, "%.*s", h ? (int)(h - path) : (int)strlen(path), path);
  size_t n;
  void *b = cw_vfs_read(file, &n);
  if (!b) return 0;
  uint64_t key = cw_hash_bytes(&(uint32_t){ PREVIEW_VERSION }, 4, 0x9e3779b97f4a7c15ull);
  if (h) key = cw_hash_bytes(h, strlen(h), key);
  key = cw_hash_bytes(b, n, key);
  if (ext_in(file, ".gltf .glb")) {
    dep_ctx d = { file, key };
    cwp_gltf_subresources(b, n, NULL, hash_dep, &d);
    key = d.key;
  } else if (ext_in(file, ".cwscene")) {
    /* a scene's look depends on its sub-scenes and resources: fold in their bytes */
    for (char *line = b; line && *line;) {
      char *nl = strchr(line, '\n');
      if (nl) *nl = 0;
      const char *ps = strstr(line, "path=\"");
      if (!strncmp(line, "[ext_scene", 10) && ps) {
        char sub[512];
        snprintf(sub, sizeof sub, "%s", ps + 6);
        char *q = strchr(sub, '"');
        if (q) *q = 0;
        size_t sn;
        void *sb = cw_vfs_read(sub, &sn);
        if (sb) key = cw_hash_bytes(sb, sn, key);
        free(sb);
      }
      for (const char *r = line; (r = strstr(r, "res(")); r += 4) {
        const char *rp = cw_res_index_path(strtoull(r + 4, NULL, 0));
        if (!rp) continue;
        char rf[512];
        const char *rh = strchr(rp, '#');
        snprintf(rf, sizeof rf, "%.*s", rh ? (int)(rh - rp) : (int)strlen(rp), rp);
        size_t rn;
        void *rb = cw_vfs_read(rf, &rn);
        if (rb) key = cw_hash_bytes(rb, rn, key);
        free(rb);
      }
      line = nl ? nl + 1 : NULL;
    }
  }
  free(b);
  return key ? key : 1;
}

static pv *cache_find(uint64_t hash) {
  for (int i = 0; i < cache.count; i++)
    if (cache.data[i].hash == hash) return &cache.data[i];
  return NULL;
}

static uint64_t hash_of(const char *path) {
  path = cw_intern(path);
  uint32_t gen = cw_vfs_generation();
  for (int i = 0; i < keys.count; i++)
    if (keys.data[i].path == path) {
      if (keys.data[i].gen != gen) {
        keys.data[i].hash = content_hash(path);
        keys.data[i].gen = gen;
      }
      return keys.data[i].hash;
    }
  path_key k = { path, content_hash(path), gen };
  cw_array_push(&keys, k);
  return k.hash;
}

/* ── off-screen rendering ─────────────────────────────────────────────── */

static cw_rt *rt;
static uint8_t *px_buf;
static size_t px_cap;

/* render a tree (pre_render + pipeline) into the shared target; returns the
 * top-down RGBA pixels (valid until the next render) */
static uint8_t *render_tree(cw_tree *t, int w, int h, bool doc) {
  if (!rt || rt->w != w || rt->h != h) {
    cw_rt_free(rt);
    rt = cw_rt_new(w, h, CW_RT_RGBA8, true);
    if (!rt) return NULL;
  }
  size_t need = (size_t)w * h * 4;
  if (px_cap < need) {
    px_buf = cw_realloc(px_buf, need, CW_MEM_EDITOR);
    px_cap = need;
  }
  cw_tree *prev = cw_current_tree();
  if (!doc) cw_set_current_tree(t);
  glBindFramebuffer(GL_FRAMEBUFFER, rt->fbo);
  glViewport(0, 0, w, h);
  glClearColor(BG[0], BG[1], BG[2], 1);
  glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
  if (cw_render3d_renderer(t)) t3_renderer_reset_bindings(cw_render3d_renderer(t));
  cw_tree_pre_render(t, 0);
  cw_pipeline *p = cw_pipeline_of(t);
  cw_pipeline_set_output(p, rt);
  cw_pipeline_render(t, w, h, 0);
  cw_pipeline_set_output(p, NULL);
  glBindFramebuffer(GL_FRAMEBUFFER, rt->fbo);
  glPixelStorei(GL_PACK_ALIGNMENT, 1);
  glReadPixels(0, 0, w, h, GL_RGBA, GL_UNSIGNED_BYTE, px_buf);
  glBindFramebuffer(GL_FRAMEBUFFER, 0);
  if (cw_render3d_renderer(t)) t3_renderer_reset_bindings(cw_render3d_renderer(t));
  if (!doc) cw_set_current_tree(prev);
  /* GL rows are bottom-up */
  uint8_t row[4096 * 4];
  for (int y = 0; y < h / 2 && w <= 4096; y++) {
    memcpy(row, px_buf + (size_t)y * w * 4, (size_t)w * 4);
    memcpy(px_buf + (size_t)y * w * 4, px_buf + (size_t)(h - 1 - y) * w * 4, (size_t)w * 4);
    memcpy(px_buf + (size_t)(h - 1 - y) * w * 4, row, (size_t)w * 4);
  }
  for (size_t i = 0; i < (size_t)w * h; i++) px_buf[i * 4 + 3] = 255;
  return px_buf;
}

static int non_background(const uint8_t *px, int w, int h) {
  int n = 0;
  for (int i = 0; i < w * h; i++) {
    int d = 0;
    for (int c = 0; c < 3; c++) {
      int x = abs((int)px[i * 4 + c] - (int)(BG[c] * 255 + 0.5f));
      d = x > d ? x : d;
    }
    n += d > 10;
  }
  return n;
}

/* the world bounds of every mesh under o */
typedef struct { t3_vec3 mn, mx; bool any; } bounds;
static void bounds_visit(t3_object *o, void *user) {
  bounds *b = user;
  if (o->type != T3_MESH && o->type != T3_SKINNED_MESH) return;
  t3_mesh *m = (t3_mesh *)o;
  if (!m->geometry) return;
  if (!m->geometry->has_bounding_box) t3_geometry_compute_bounding_box(m->geometry);
  t3_box3 bb = m->geometry->bounding_box;
  for (int k = 0; k < 8; k++) {
    t3_vec3 p = { k & 1 ? bb.max.x : bb.min.x, k & 2 ? bb.max.y : bb.min.y, k & 4 ? bb.max.z : bb.min.z };
    p = t3_vec3_apply_mat4(p, &o->matrix_world);
    if (!b->any) b->mn = b->mx = p, b->any = true;
    b->mn.x = fminf(b->mn.x, p.x), b->mn.y = fminf(b->mn.y, p.y), b->mn.z = fminf(b->mn.z, p.z);
    b->mx.x = fmaxf(b->mx.x, p.x), b->mx.y = fmaxf(b->mx.y, p.y), b->mx.z = fmaxf(b->mx.z, p.z);
  }
}

/* aim cam (a Camera3D node) at the bounds from direction dir */
static void frame_bounds(cw_node cam, const bounds *b, t3_vec3 dir, float fov_deg) {
  t3_vec3 c = { (b->mn.x + b->mx.x) * 0.5f, (b->mn.y + b->mx.y) * 0.5f, (b->mn.z + b->mx.z) * 0.5f };
  float r = 0.5f * sqrtf((b->mx.x - b->mn.x) * (b->mx.x - b->mn.x) + (b->mx.y - b->mn.y) * (b->mx.y - b->mn.y) +
                         (b->mx.z - b->mn.z) * (b->mx.z - b->mn.z));
  if (r < 1e-4f) r = 1;
  float l = sqrtf(dir.x * dir.x + dir.y * dir.y + dir.z * dir.z);
  float dist = r / sinf(fov_deg * 0.5f * 3.14159265f / 180) * 1.05f;
  cw_node_set(cam, "position", cw_val_vec3(c.x + dir.x / l * dist, c.y + dir.y / l * dist, c.z + dir.z / l * dist));
  cw_node_set(cam, "near", cw_val_float(dist * 0.02f));
  cw_node_set(cam, "far", cw_val_float(dist * 4 + r * 4));
  cw_node3d_look_at(cam, c, (t3_vec3){ 0, 1, 0 });
}

/* the thumbnail tree: environment, light, camera, and one subject at a time */
static cw_tree *ptree;
static cw_node pcam, psubject;

static cw_node add_stage(cw_node root, bool camera) {
  cw_node env = cw_node_new("WorldEnvironment", "PreviewEnv");
  cw_node_set(env, "background", cw_val_color(BG[0], BG[1], BG[2], 1));
  cw_node_set(env, "ambient_energy", cw_val_float(0.45f));
  cw_node_set(env, "hemi_energy", cw_val_float(0.5f));
  cw_node_add_child(root, env);
  cw_node sun = cw_node_new("DirectionalLight3D", "PreviewSun");
  cw_node_set(sun, "rotation", cw_val_vec3(-0.9f, 0.7f, 0));
  cw_node_set(sun, "energy", cw_val_float(2.2f));
  cw_node_add_child(root, sun);
  if (!camera) return (cw_node){ 0 };
  cw_node cam = cw_node_new("Camera3D", "PreviewCamera");
  cw_node_set(cam, "fov", cw_val_float(40));
  cw_node_add_child(root, cam);
  return cam;
}

static void ensure_ptree(void) {
  if (ptree) return;
  cw_tree *prev = cw_current_tree();
  ptree = cw_tree_new();
  cw_set_current_tree(ptree);
  pcam = add_stage(cw_tree_root(ptree), true);
  cw_set_current_tree(prev);
}

static void fill_texture(pv *e, const uint8_t *px, int w, int h) {
  if (CW_HANDLE_IS_NULL(e->tex)) e->tex = cw_texture_from_pixels(w, h, px, &(cw_texture_desc){ .filter = CW_FILTER_LINEAR });
  else cw_texture_update(e->tex, px);
}

/* one preview: true when it rendered (e->error says why not) */
static bool render_one(const char *path, pv *e) {
  const char *kind = kind_of(path);
  e->error[0] = 0;
  if (!kind) {
    snprintf(e->error, sizeof e->error, "%s has no preview", path);
    return false;
  }
  char file[512];
  const char *hash = strchr(path, '#');
  snprintf(file, sizeof file, "%.*s", hash ? (int)(hash - path) : (int)strlen(path), path);
  if (!strcmp(kind, "scene")) {
    /* a fresh tree: the scene brings its own environment and camera */
    cw_tree *prev = cw_current_tree();
    cw_tree *t = cw_tree_new();
    cw_set_current_tree(t);
    cw_node inst = cw_scene_instance_path(file);
    bool ok = !CW_HANDLE_IS_NULL(inst);
    uint8_t *px = NULL;
    if (ok) {
      cw_node_add_child(cw_tree_root(t), inst);
      bool has_cam = false, has_env = false, has_light = false;
      /* look for what the scene brings */
      cw_node stack[256];
      int sp = 0;
      stack[sp++] = inst;
      while (sp) {
        cw_node n = stack[--sp];
        has_cam |= cw_node_is(n, "Camera3D");
        has_env |= cw_node_is(n, "WorldEnvironment");
        has_light |= cw_node_is(n, "Light3D") || cw_node_is(n, "DirectionalLight3D");
        for (int i = 0; i < cw_node_child_count(n) && sp < 256; i++) stack[sp++] = cw_node_child(n, i);
      }
      cw_node cam = (cw_node){ 0 };
      if (!has_cam || !has_env || !has_light) {
        cw_node stage = cw_node_new("Node3D", "PreviewStage");
        cw_node_add_child(cw_tree_root(t), stage);
        cam = add_stage(stage, !has_cam);
        if (has_env) cw_node_free(cw_node_get(stage, "PreviewEnv"));
      }
      if (!has_cam && cw_node_is(inst, "Node3D")) {
        cw_tree_pre_render(t, 0);
        t3_object *o = cw_node3d_object(inst);
        bounds b = { 0 };
        if (o) {
          t3_object_update_world_matrix(o, true, true);
          t3_object_traverse(o, bounds_visit, &b);
        }
        if (b.any) frame_bounds(cam, &b, (t3_vec3){ 1, 0.8f, 1.3f }, 40);
      }
      if (cw_node_is(inst, "Node3D") || has_cam) px = render_tree(t, THUMB_PX, THUMB_PX, false);
      else snprintf(e->error, sizeof e->error, "%s is not a 3D scene (its root is a %s)", file, cw_node_get_type(inst)->name);
    } else {
      snprintf(e->error, sizeof e->error, "%s did not load (the log says why)", file);
    }
    if (px) {
      fill_texture(e, px, THUMB_PX, THUMB_PX);
      e->coverage = (float)non_background(px, THUMB_PX, THUMB_PX) / (THUMB_PX * THUMB_PX);
    }
    cw_set_current_tree(prev);
    cw_tree_free(t);
    return px != NULL;
  }
  /* a file whose import failed has nothing trustworthy to show: say why */
  char *ij = NULL;
  if (cwed_run(cw_frame_printf("import.info path=%s", cwed_quote(file)), &ij) && ij && strstr(ij, "\"status\":\"error\"")) {
    snprintf(e->error, sizeof e->error, "%s did not import (import.info %s says why)", file, file);
    cw_free(ij);
    return false;
  }
  cw_free(ij);
  /* models, meshes, materials, textures: the thumbnail tree */
  ensure_ptree();
  cw_tree *prev = cw_current_tree();
  cw_set_current_tree(ptree);
  if (cw_node_valid(psubject)) cw_node_free(psubject);
  psubject = cw_node_new("Node3D", "Subject");
  cw_node_add_child(cw_tree_root(ptree), psubject);
  t3_object *holder = cw_node3d_object(psubject);
  cw_res res = cw_res_load(path, 0);
  bool ok = cw_res_get_state(res) == CW_RES_STATE_READY;
  t3_vec3 dir = { 1, 0.75f, 1.35f };
  if (!ok) {
    snprintf(e->error, sizeof e->error, "%s did not load (the log says why)", path);
  } else if (!strcmp(kind, "model")) {
    t3_object *o = cw_gltf_instantiate(res);
    if (o) { t3_object_add(holder, o); t3_release(o); }
    ok = o != NULL;
  } else if (!strcmp(kind, "mesh")) {
    const cw_mesh *m = cw_res_mesh(res);
    t3_mesh *o = m ? t3_mesh_new(m->lod[0], m->material) : NULL;
    if (o) { t3_object_add(holder, o); t3_release(o); }
    ok = o != NULL;
  } else if (!strcmp(kind, "material")) {
    t3_material *mat = cw_res_material(res);
    t3_geometry *g = t3_sphere_geometry_new(1, 48, 24);
    t3_mesh *o = mat ? t3_mesh_new(g, mat) : NULL;
    t3_release(g);
    if (o) { t3_object_add(holder, o); t3_release(o); }
    ok = o != NULL;
  } else if (!strcmp(kind, "texture")) {
    t3_texture *tx = cw_res_texture(res);
    cw_texture_info ti;
    float aspect = cw_res_texture_info(res, &ti) && ti.width > 0 ? (float)ti.height / (float)ti.width : 1;
    t3_geometry *g = t3_plane_geometry_new(2, 2 * aspect, 1, 1);
    t3_material *mat = t3_mesh_basic_material_new(0xffffff);
    t3_material_set_map(mat, tx);
    t3_mesh *o = tx ? t3_mesh_new(g, mat) : NULL;
    t3_release(g);
    t3_release(mat);
    if (o) { t3_object_add(holder, o); t3_release(o); }
    ok = o != NULL;
    dir = (t3_vec3){ 0, 0, 1 };
  }
  if (ok) {
    t3_object_update_world_matrix(holder, true, true);
    bounds b = { 0 };
    t3_object_traverse(holder, bounds_visit, &b);
    if (!b.any) {
      ok = false;
      snprintf(e->error, sizeof e->error, "%s has nothing to draw", path);
    } else {
      frame_bounds(pcam, &b, dir, 40);
    }
  } else if (!e->error[0]) {
    snprintf(e->error, sizeof e->error, "%s is not a %s", path, kind);
  }
  cw_set_current_tree(prev);
  uint8_t *px = ok ? render_tree(ptree, THUMB_PX, THUMB_PX, false) : NULL;
  cw_res_release(res);
  cw_set_current_tree(ptree);
  cw_node_free(psubject);
  psubject = (cw_node){ 0 };
  cw_set_current_tree(prev);
  if (px) {
    fill_texture(e, px, THUMB_PX, THUMB_PX);
    e->coverage = (float)non_background(px, THUMB_PX, THUMB_PX) / (THUMB_PX * THUMB_PX);
  }
  return px != NULL;
}

static uint64_t use_clock;
static void evict(uint64_t keep) {
  while (cache.count > MAX_CACHED) {
    int old = -1;
    for (int i = 0; i < cache.count; i++)
      if (cache.data[i].hash != keep && (old < 0 || cache.data[i].last_use < cache.data[old].last_use)) old = i;
    if (old < 0) return;
    if (!CW_HANDLE_IS_NULL(cache.data[old].tex)) cw_texture_destroy(cache.data[old].tex);
    cache.data[old] = cache.data[--cache.count];
  }
}

static pv *request(const char *path, bool now) {
  uint64_t h = hash_of(path);
  if (!h) return NULL;
  evict(h);
  pv *e = cache_find(h);
  if (e) e->last_use = ++use_clock;
  if (e && (e->ready || e->error[0]) && !now) return e;
  if (!e) {
    pv n = { .hash = h, .last_use = ++use_clock };
    cw_array_push(&cache, n);
    e = &cache.data[cache.count - 1];
  }
  if (now) {
    e->ready = render_one(path, e);
    renders++;
    return e;
  }
  const char *p = cw_intern(path);
  for (int i = 0; i < queue.count; i++)
    if (queue.data[i] == p) return e;
  cw_array_push(&queue, p);
  return e;
}

static void pump(void) {
  /* one render a frame, oldest request first */
  while (queue.count) {
    const char *p = queue.data[0];
    memmove(queue.data, queue.data + 1, sizeof *queue.data * (size_t)(--queue.count));
    uint64_t h = hash_of(p);
    pv *e = h ? cache_find(h) : NULL;
    if (!e || e->ready || e->error[0]) continue;
    e->ready = render_one(p, e);
    renders++;
    if (!e->ready) cwed_log(CWED_LOG_WARN, "preview of %s: %s", p, e->error);
    break;
  }
}

/* ── commands ──────────────────────────────────────────────────────────── */

static void reply_pv(cwed_call *c, const char *path, const pv *e) {
  cwed_json *j = cwed_reply(c);
  cwed_json_key(j, "path");
  cwed_json_str(j, path);
  cwed_json_key(j, "hash");
  cwed_json_str(j, cw_frame_printf("0x%016llx", (unsigned long long)e->hash));
  cwed_json_key(j, "ready");
  cwed_json_raw(j, e->ready ? "true" : "false");
  cwed_json_key(j, "texture");
  cwed_json_printf(j, "%llu", (unsigned long long)e->tex.id);
  cwed_json_key(j, "size");
  cwed_json_printf(j, "%d", THUMB_PX);
  cwed_json_key(j, "coverage");
  cwed_json_printf(j, "%.4f", e->coverage);
  if (e->error[0]) {
    cwed_json_key(j, "error");
    cwed_json_str(j, e->error);
  }
}

static bool get_exec(cwed_call *c) {
  const char *path = cwed_arg(c, "path");
  if (!kind_of(path)) return cwed_fail(c, "%s has no preview (models, meshes, materials, textures and scenes do)", path);
  pv *e = request(path, false);
  if (!e) return cwed_fail(c, "no file '%s' in the project", path);
  reply_pv(c, path, e);
  cwed_json_key(cwed_reply(c), "queued");
  cwed_json_printf(cwed_reply(c), "%d", queue.count);
  return true;
}

static bool render_exec(cwed_call *c) {
  const char *path = cwed_arg(c, "path");
  if (!kind_of(path)) return cwed_fail(c, "%s has no preview (models, meshes, materials, textures and scenes do)", path);
  pv *e = request(path, true);
  if (!e) return cwed_fail(c, "no file '%s' in the project", path);
  if (!e->ready) return cwed_fail(c, "%s", e->error[0] ? e->error : "the preview did not render");
  reply_pv(c, path, e);
  return true;
}

/* the active scene through its own camera */

static uint8_t *render_doc(cwed_doc *d, int w, int h) {
  cwed_doc_enter(d);
  uint8_t *px = cw_render3d_camera(cwed_doc_tree(d)).id ? render_tree(cwed_doc_tree(d), w, h, true) : NULL;
  cwed_doc_leave();
  return px;
}

static bool scene_exec(cwed_call *c) {
  cwed_doc *d = cwed_doc_active();
  if (!d) return cwed_fail(c, "no scene is open (scene.open)");
  int w = (int)cwed_arg_int(c, "width"), h = (int)cwed_arg_int(c, "height");
  if (w < 16 || h < 16 || w > 2048 || h > 2048) return cwed_fail(c, "width and height are 16..2048");
  uint8_t *px = render_doc(d, w, h);
  if (!px) return cwed_fail(c, "the scene has no current Camera3D to render through");
  cwed_json *j = cwed_reply(c);
  cwed_json_key(j, "pixels");
  cwed_json_printf(j, "%d", non_background(px, w, h));
  cwed_json_key(j, "hash");
  cwed_json_printf(j, "%u", cw_hash32(px, (size_t)w * h * 4, CW_HASH32_INIT));
  return true;
}

static int last_node_pixels = -1;
static bool node_pixels_exec(cwed_call *c) {
  cwed_doc *d = cwed_call_doc(c);
  cw_node n = cwed_arg_node(c, "path");
  if (CW_HANDLE_IS_NULL(n)) return false;
  if (!cw_node_is(n, "Node3D")) return cwed_fail(c, "%s is not a Node3D", cwed_arg(c, "path"));
  int w = (int)cwed_arg_int(c, "width"), h = (int)cwed_arg_int(c, "height");
  if (w < 16 || h < 16 || w > 2048 || h > 2048) return cwed_fail(c, "width and height are 16..2048");
  uint8_t *a = render_doc(d, w, h);
  if (!a) return cwed_fail(c, "the scene has no current Camera3D to render through");
  size_t sz = (size_t)w * h * 4;
  uint8_t *with = cw_alloc(sz, CW_MEM_EDITOR);
  memcpy(with, a, sz);
  /* a measurement, not an edit: hidden for one render, then restored (no command, nothing to undo) */
  cw_value vis;
  cw_node_get_prop(n, "visible", &vis);
  cw_node_set(n, "visible", cw_val_bool(false));
  uint8_t *b = render_doc(d, w, h);
  cw_node_set(n, "visible", vis);
  int diff = 0;
  for (size_t i = 0; b && i < (size_t)w * h; i++) {
    int m = 0;
    for (int k = 0; k < 3; k++) {
      int x = abs((int)with[i * 4 + k] - (int)b[i * 4 + k]);
      m = x > m ? x : m;
    }
    diff += m > 10;
  }
  cw_free(with);
  last_node_pixels = diff;
  cwed_json_key(cwed_reply(c), "pixels");
  cwed_json_printf(cwed_reply(c), "%d", diff);
  cwed_json_key(cwed_reply(c), "of");
  cwed_json_printf(cwed_reply(c), "%d", w * h);
  return true;
}

/* ── the Preview panel ─────────────────────────────────────────────────── */

static cw_node panel, view, caption;
static cw_texture view_tex;
static int view_w, view_h;
static uint32_t view_version, view_doc_gen;
static cwed_doc *view_doc;
static bool view_dirty = true;
static t3_mat4 view_cam_world, view_cam_proj_inv;
static bool view_cam_valid;

static void refresh_view(void) {
  cwed_doc *d = cwed_doc_active();
  cw_rect r = cw_control_global_rect(view);
  int w = (int)r.w, h = (int)r.h;
  if (w < 32 || h < 32) return;
  if (w > 2048) w = 2048;
  if (h > 2048) h = 2048;
  if (!view_dirty && d == view_doc && d && cwed_doc_version(d) == view_version && w == view_w && h == view_h &&
      view_doc_gen == cw_vfs_generation())
    return;
  view_dirty = false;
  view_doc = d;
  view_version = d ? cwed_doc_version(d) : 0;
  view_doc_gen = cw_vfs_generation();
  uint8_t *px = d ? render_doc(d, w, h) : NULL;
  view_cam_valid = false;
  if (!px) {
    cw_label_set_text(caption, d ? "This scene has no current Camera3D: add one to preview it here." : "No scene open.");
    cw_control_set_visible(view, false);
    return;
  }
  cw_node camn = cw_render3d_camera(cwed_doc_tree(d));
  t3_camera *cam = cw_camera3d_t3(camn);
  if (cam) {
    view_cam_world = cam->base.matrix_world;
    view_cam_proj_inv = cam->projection_matrix_inverse;
    view_cam_valid = true;
  }
  cw_control_set_visible(view, true);
  if (CW_HANDLE_IS_NULL(view_tex) || w != view_w || h != view_h) {
    view_tex = cw_texture_from_pixels(w, h, px, &(cw_texture_desc){ .filter = CW_FILTER_LINEAR, .standalone = true });
    cw_texture_rect_set_texture(view, view_tex);
  } else {
    cw_texture_update(view_tex, px);
  }
  view_w = w, view_h = h;
  cw_label_set_text(caption, cw_frame_printf("%s through %s  (drop a model or scene here to place it on the ground)",
                                             cwed_doc_title(d), cw_node_name(camn)));
}

/* the ground point (y = 0) under a point of the view, through the scene's camera */
static bool ground_at(cw_vec2 local, t3_vec3 *out) {
  if (!view_cam_valid || view_w <= 0) return false;
  t3_vec3 ndc = { local.x / (float)view_w * 2 - 1, 1 - local.y / (float)view_h * 2, 0.5f };
  t3_vec3 pv = t3_vec3_apply_mat4(ndc, &view_cam_proj_inv);
  t3_vec3 pw = t3_vec3_apply_mat4(pv, &view_cam_world);
  t3_vec3 o = { view_cam_world.e[12], view_cam_world.e[13], view_cam_world.e[14] };
  t3_vec3 dir = { pw.x - o.x, pw.y - o.y, pw.z - o.z };
  if (fabsf(dir.y) < 1e-6f) return false;
  float t = -o.y / dir.y;
  if (t <= 0) return false;
  *out = (t3_vec3){ o.x + dir.x * t, 0, o.z + dir.z * t };
  return true;
}

static bool view_can_drop(cw_node target, const cw_ui_drag *d, cw_vec2 local, cw_rect *hl, void *user) {
  (void)target, (void)local, (void)hl, (void)user;
  const char *k = d->text ? kind_of(d->text) : NULL;
  return cwed_doc_active() && k && (!strcmp(k, "model") || !strcmp(k, "scene") || !strcmp(k, "mesh"));
}
static void view_drop(cw_node target, const cw_ui_drag *d, cw_vec2 local, void *user) {
  (void)user;
  /* local is relative to the panel; the image sits in `view` */
  cw_rect pr = cw_control_global_rect(target), vr = cw_control_global_rect(view);
  cw_vec2 in = { local.x + pr.x - vr.x, local.y + pr.y - vr.y };
  t3_vec3 g;
  if (ground_at(in, &g))
    cwed_runf("fs.place file=%s position=\"vec3(%.2f, 0, %.2f)\"", cwed_quote(d->text), (double)g.x, (double)g.z);
  else cwed_runf("fs.place file=%s", cwed_quote(d->text));
}

static cw_node create(void *user) {
  (void)user;
  panel = cw_node_new("VBoxContainer", NULL);
  caption = cw_node_new("Label", "Caption");
  cw_node_set(caption, "ellipsis", cw_val_bool(true));
  cw_node_set(caption, "theme_overrides", cw_val_string("font_size = 12; font_color = #8a93a3"));
  view = cw_node_new("TextureRect", "View");
  cw_node_set(view, "stretch_mode", cw_val_int(0));
  cw_control_set_size_flags(view, CW_SIZE_EXPAND_FILL, CW_SIZE_EXPAND_FILL);
  cw_control_set_mouse_filter(view, CW_MOUSE_PASS);
  cw_node_add_child(panel, caption);
  cw_node_add_child(panel, view);
  cwed_drop_add(panel, &(cwed_drop_desc){ .kind = "file", .can_drop = view_can_drop, .drop = view_drop });
  return panel;
}

static void on_event(const cwed_event *e, void *user) {
  (void)e, (void)user;
  view_dirty = true;
}

static void frame(double dt) {
  (void)dt;
  pump();
  if (cw_node_valid(view) && cwed_panel_visible("preview") && cw_control_visible_in_tree(panel)) refresh_view();
  f_renders = renders;
  f_queued = queue.count;
}

static void shutdown(void) {
  if (ptree) cw_tree_free(ptree);
  ptree = NULL;
  cw_rt_free(rt);
  rt = NULL;
}

static void init(void) {
  cwed_panel_register(&(cwed_panel_desc){ .id = "preview", .title = "Preview", .slot = "center", .order = 60, .create = create });
  cwed_subscribe(CWED_EV_DOC_ACTIVE | CWED_EV_DOC_CHANGED | CWED_EV_DOC_OPENED | CWED_EV_FILES, on_event, NULL);
  cw_debug_i32("preview.renders", &f_renders);
  cw_debug_i32("preview.queued", &f_queued);
  cw_debug_i32("preview.node_pixels", &last_node_pixels);
  static const cwed_param path_p[] = { { "path", CWED_ARG_STRING, NULL, "a model, texture or scene (or file.glb#mesh/0/0, #material/1)" }, { 0 } };
  static const cwed_param scene_p[] = {
    { "width", CWED_ARG_INT, "320", "pixels" },
    { "height", CWED_ARG_INT, "180", "pixels" },
    { 0 } };
  static const cwed_param node_p[] = {
    { "path", CWED_ARG_PATH, NULL, "a Node3D in the active scene" },
    { "width", CWED_ARG_INT, "320", "pixels" },
    { "height", CWED_ARG_INT, "180", "pixels" },
    { 0 } };
  cwed_command_register(&(cwed_command_desc){ .name = "preview.get", .title = "Resource Preview", .params = path_p,
    .flags = CWED_CMD_QUERY | CWED_CMD_HIDDEN,
    .doc = "A file's thumbnail: content hash, ready, the UI texture id, coverage (share of non-background pixels), or the "
           "error; queues the render (one per frame).", .exec = get_exec });
  cwed_command_register(&(cwed_command_desc){ .name = "preview.render", .title = "Render Preview Now", .params = path_p,
    .flags = CWED_CMD_HIDDEN, .doc = "Render a file's thumbnail now, bypassing the queue and the cache.", .exec = render_exec });
  cwed_command_register(&(cwed_command_desc){ .name = "preview.scene", .title = "Render Active Scene", .params = scene_p,
    .flags = CWED_CMD_QUERY | CWED_CMD_HIDDEN,
    .doc = "Render the active scene through its camera off-screen: non-background pixels and an image hash.", .exec = scene_exec });
  cwed_command_register(&(cwed_command_desc){ .name = "preview.node_pixels", .title = "Pixels a Node Covers", .params = node_p,
    .flags = CWED_CMD_QUERY | CWED_CMD_HIDDEN | CWED_CMD_SCENE,
    .doc = "How many pixels a node covers in the active scene's render (rendered again with it hidden; nothing changes).",
    .exec = node_pixels_exec });
}

CWED_MODULE(previews) = { .title = "Previews", .milestone = "M4.2", .init = init, .frame = frame, .shutdown = shutdown };

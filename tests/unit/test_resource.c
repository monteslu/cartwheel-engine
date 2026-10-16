/* Resource registry suite (): types, paths, UIDs, reference
 * counts, the deferred-free queue, async completion order, sub-resources,
 * glTF instancing and LODs, in-place reloads. Assets come from cwpack
 * (tests/unit/res_project/roots.txt), so this also proves the .cwpak reader
 * natively and the cart bundle in carts. */
#include <stdlib.h>

#include "cartwheel/resource.h"
#include "test.h"
#include "three.h"
#include "../../src/audio/internal.h" /* the audio module's decoder, to listen to an import */
#include <math.h>

/* ── a type of our own: ".ord" files decode slowly, by their content ── */
static cw_res_type order_type;
static int finish_order[16], finish_n;
static int cb_order[16], cb_n;

static void *ord_decode(const cw_res_decode_args *a, size_t *cost) {
  long spin = strtol((const char *)a->bytes, NULL, 10);
  volatile uint32_t x = 1;
  for (long i = 0; i < spin; i++) x = x * 1664525u + 1013904223u; /* earlier files take longer */
  int *v = cw_alloc(sizeof *v, CW_MEM_GAME);
  *v = a->path[6] - 'a'; /* "order/<letter>.ord" */
  *cost = 1;
  return v;
}
static void *ord_finish(void *decoded, cw_res self, const cw_res *deps, int n, void *u) {
  finish_order[finish_n++] = *(int *)decoded;
  return decoded;
}
static void ord_unload(void *p, void *u) { cw_free(p); }
static void ord_done(cw_res r, cw_res_state st, void *user) {
  if (st == CW_RES_STATE_READY) cb_order[cb_n++] = *(int *)cw_res_get(r);
}

static void test_types(void) {
  CHECK_EQ_INT(cw_res_type_find("texture"), CW_RES_TEXTURE);
  CHECK_EQ_INT(cw_res_type_find("gltf"), CW_RES_GLTF);
  CHECK_EQ_INT(cw_res_type_find("audio_clip"), CW_RES_AUDIO_CLIP);
  CHECK_EQ_INT(cw_res_type_find("binary"), CW_RES_BINARY);
  cw_res_type_desc d = { .name = "unit_order", .extensions = ".ord", .decode = ord_decode, .finish = ord_finish,
                         .unload = ord_unload, .discard = ord_unload };
  order_type = cw_res_register_type(&d);
  CHECK(order_type >= CW_RES_BUILTIN_COUNT);
  CHECK_EQ_INT(cw_res_register_type(&d), order_type); /* same name: same id */
  CHECK_EQ_INT(cw_res_type_find("unit_order"), order_type);
  CHECK(!strcmp(cw_res_type_name(CW_RES_MESH), "mesh"));
}

static void test_paths_and_uids(void) {
  cw_res a = cw_res_load("res://text/hello.txt", 0);
  cw_res b = cw_res_load("./text//hello.txt", 0);
  cw_res c = cw_res_load("text/hello.txt", CW_RES_TEXT);
  CHECK(a.id == b.id && b.id == c.id);
  CHECK_EQ_INT(cw_res_ref_count(a), 3);
  CHECK(cw_res_is_ready(a));
  const cw_blob *bl = cw_res_blob(a);
  CHECK(bl && bl->len == 16 && !memcmp(bl->data, "hello cartwheel\n", 16));
  CHECK(!strcmp(cw_res_path(a), "text/hello.txt"));
  CHECK(cw_res_uid(a) == 0x1234abcd5678ef00ull); /* from the sidecar, through res.index */
  CHECK(cw_res_find_uid(0x1234abcd5678ef00ull).id == a.id);
  cw_res u = cw_res_load_uid(0x1234abcd5678ef00ull, 0);
  CHECK(u.id == a.id);
  CHECK_EQ_INT(cw_res_ref_count(a), 4);
  /* a file without a sidecar: the path hash */
  cw_res j = cw_res_load("data/config.json", 0);
  CHECK(cw_res_uid(j) == cw_res_path_uid("data/config.json"));
  CHECK_EQ_INT(cw_res_type_of(j), CW_RES_JSON);
  uint64_t uid = cw_res_uid(j);
  FOLD(uid);
  for (int i = 0; i < 4; i++) cw_res_release(a);
  cw_res_release(j);
}

static void test_deferred_free(void) {
  cw_res_stats s0;
  cw_res_get_stats(&s0);
  cw_res a = cw_res_load("text/hello.txt", 0);
  CHECK_EQ_INT(cw_res_ref_count(a), 1);
  cw_res_release(a);
  /* the last reference went: still there for CW_RES_FREE_DELAY_FRAMES frames */
  CHECK(cw_res_find("text/hello.txt").id == a.id);
  CHECK(cw_res_is_ready(a));
  cw_res_frame();
  CHECK(cw_res_is_ready(a));
  cw_res b = cw_res_load("text/hello.txt", 0); /* revived: the same resource, not reloaded */
  CHECK(b.id == a.id);
  CHECK_EQ_INT(cw_res_version(b), 1);
  cw_res_release(b);
  for (int i = 0; i < CW_RES_FREE_DELAY_FRAMES; i++) cw_res_frame();
  CHECK_EQ_INT(cw_res_get_state(a), CW_RES_STATE_NONE); /* stale handle */
  CHECK(cw_res_find("text/hello.txt").id == 0);
  cw_res_stats s1;
  cw_res_get_stats(&s1);
  CHECK(s1.freed > s0.freed);
}

static void test_failures(void) {
  cw_res m = cw_res_load("nope/missing.txt", 0);
  CHECK(!CW_HANDLE_IS_NULL(m));
  CHECK_EQ_INT(cw_res_get_state(m), CW_RES_STATE_FAILED);
  CHECK(cw_res_get(m) == NULL);
  cw_res bad = cw_res_load("data/bad.json", 0);
  CHECK_EQ_INT(cw_res_get_state(bad), CW_RES_STATE_FAILED);
  cw_res esc = cw_res_load("../outside.txt", 0);
  CHECK_EQ_INT(cw_res_get_state(esc), CW_RES_STATE_FAILED);
  cw_res sub = cw_res_load("models/box.glb#mesh/9/9", 0);
  CHECK_EQ_INT(cw_res_get_state(sub), CW_RES_STATE_FAILED);
  cw_res_release(m);
  cw_res_release(bad);
  cw_res_release(esc);
  cw_res_release(sub);
}

static void test_async_order(void) {
  cw_res_set_upload_budget(1, 0); /* one finish per frame */
  cw_res h[8];
  char p[32];
  for (int i = 0; i < 8; i++) {
    snprintf(p, sizeof p, "order/%c.ord", 'a' + i);
    h[i] = cw_res_load_async(p, 0, ord_done, NULL);
    CHECK_EQ_INT(cw_res_get_state(h[i]), CW_RES_STATE_LOADING);
  }
  int frames = 0, max_per_frame = 0;
  while (finish_n < 8 && frames < 100000) {
    cw_res_frame();
    cw_res_stats s;
    cw_res_get_stats(&s);
    if (s.finished_last_frame > max_per_frame) max_per_frame = s.finished_last_frame;
    frames++;
  }
  CHECK_EQ_INT(finish_n, 8);
  CHECK_EQ_INT(cb_n, 8);
  if (max_per_frame != 1) cw_error("max_per_frame %d frames %d", max_per_frame, frames);
  CHECK(max_per_frame == 1);
  bool in_order = true;
  for (int i = 0; i < 8; i++) in_order &= finish_order[i] == i && cb_order[i] == i;
  CHECK(in_order); /* request order, although h decoded first */
  FOLD(finish_order);
  /* a cache hit's callback comes at the next frame, never inside the call */
  int before = cb_n;
  cw_res again = cw_res_load_async("order/c.ord", 0, ord_done, NULL);
  CHECK_EQ_INT(cb_n, before);
  cw_res_frame();
  CHECK_EQ_INT(cb_n, before + 1);
  CHECK(again.id == h[2].id);
  cw_res_release(again);
  for (int i = 0; i < 8; i++) cw_res_release(h[i]);
  cw_res_set_upload_budget(32u << 20, 4);
}


static void flush_frees(void) {
  for (int i = 0; i < CW_RES_FREE_DELAY_FRAMES + 2; i++) cw_res_frame();
}

/* lanes and priorities: finish order is (lane, -priority, request order),
 * and a repeated request with a better key moves a load up */
static void test_priorities(void) {
  flush_frees();
  cw_res_set_upload_budget(1, 0); /* one finish per frame */
  static const int lane[8] = { 0, 0, 1, 0, 0, 1, 0, 0 }, prio[8] = { 0, 2, 5, 2, -1, 0, 9, 0 };
  finish_n = cb_n = 0;
  cw_res h[8];
  char p[32];
  for (int i = 0; i < 8; i++) {
    snprintf(p, sizeof p, "order/%c.ord", 'a' + i);
    h[i] = cw_res_load_async_ex(p, 0, &(cw_res_load_opts){ .lane = lane[i], .priority = prio[i], .done = ord_done });
    CHECK_EQ_INT(cw_res_get_state(h[i]), CW_RES_STATE_LOADING);
  }
  /* e (lane 0, -1) asks again at 10: now first */
  cw_res e2 = cw_res_load_async_ex("order/e.ord", 0, &(cw_res_load_opts){ .priority = 10 });
  CHECK(e2.id == h[4].id);
  /* f asks again lower: stays where it is */
  cw_res f2 = cw_res_load_async_ex("order/f.ord", 0, &(cw_res_load_opts){ .lane = 3 });
  int frames = 0;
  while (finish_n < 8 && frames < 100000) cw_res_frame(), frames++;
  static const int want[8] = { 4, 6, 1, 3, 0, 7, 2, 5 }; /* e g b d a h | c f */
  bool ok = finish_n == 8;
  for (int i = 0; i < 8 && ok; i++) ok &= finish_order[i] == want[i] && cb_order[i] == want[i];
  if (!ok) cw_error("priority order: %d %d %d %d %d %d %d %d", finish_order[0], finish_order[1], finish_order[2],
                    finish_order[3], finish_order[4], finish_order[5], finish_order[6], finish_order[7]);
  CHECK(ok);
  FOLD(finish_order);
  cw_res_release(e2);
  cw_res_release(f2);
  for (int i = 0; i < 8; i++) cw_res_release(h[i]);
  cw_res_set_upload_budget(32u << 20, 4);
  flush_frees();
}

/* releasing a load in flight cancels it without waiting; requesting it again later loads it afresh */
static void test_cancel(void) {
  flush_frees();
  finish_n = cb_n = 0;
  cw_res_stats s0, s1;
  cw_res_get_stats(&s0);
  cw_res a = cw_res_load_async("order/a.ord", 0, ord_done, NULL); /* the slowest decode */
  cw_res b = cw_res_load_async("order/h.ord", 0, ord_done, NULL);
  cw_res_release(a);
  int frames = 0;
  while (cb_n < 1 && frames < 100000) cw_res_frame(), frames++;
  CHECK_EQ_INT(cb_n, 1);
  CHECK_EQ_INT(cb_order[0], 7); /* only h finished */
  flush_frees();
  for (int i = 0; i < 100000 && !CW_HANDLE_IS_NULL(cw_res_find("order/a.ord")); i++) cw_res_frame(); /* its decode lands, then it goes */
  cw_res_get_stats(&s1);
  CHECK_EQ_INT(s1.cancelled - s0.cancelled, 1);
  CHECK(CW_HANDLE_IS_NULL(cw_res_find("order/a.ord"))); /* freed */
  CHECK_EQ_INT(s1.in_flight, 0);
  cw_res a2 = cw_res_load_async("order/a.ord", 0, ord_done, NULL);
  CHECK_EQ_INT(cw_res_wait(a2), CW_RES_STATE_READY);
  CHECK_EQ_INT(cb_n, 2);
  cw_res_release(a2);
  cw_res_release(b);
  flush_frees();
}

/* the residency cache keeps released resources loaded within its budget, least recently released out first */
static void test_residency(void) {
  flush_frees();
  cw_res_stats s0, s1;
  cw_res a = cw_res_load("order/a.ord", 0), b = cw_res_load("order/b.ord", 0), c = cw_res_load("order/c.ord", 0);
  cw_res_info ia;
  CHECK(cw_res_get_info(a, &ia));
  size_t each = ia.file_bytes; /* 7 bytes + newline: the same for every .ord file */
  cw_res_set_residency_budget(each * 2);
  cw_res_release(a);
  cw_res_release(b);
  cw_res_release(c);
  cw_res_get_stats(&s0);
  CHECK_EQ_INT(s0.cached, 3);
  flush_frees();
  cw_res_get_stats(&s1);
  CHECK_EQ_INT(s1.cached, 2);
  CHECK_EQ_INT(s1.evicted - s0.evicted, 1);
  CHECK(CW_HANDLE_IS_NULL(cw_res_find("order/a.ord")));  /* the oldest went */
  CHECK(cw_res_is_ready(cw_res_find("order/b.ord")));    /* still loaded, no references */
  CHECK_EQ_INT(cw_res_ref_count(cw_res_find("order/c.ord")), 0);
  uint64_t decoded = s1.decoded;
  cw_res b2 = cw_res_load_async("order/b.ord", 0, NULL, NULL); /* a cache hit: no read, no decode */
  CHECK(cw_res_is_ready(b2));
  cw_res_get_stats(&s1);
  CHECK_EQ_INT(s1.decoded, decoded);
  CHECK_EQ_INT(s1.cached, 1);
  cw_res_set_residency_budget(0); /* everything cached goes */
  flush_frees();
  CHECK(CW_HANDLE_IS_NULL(cw_res_find("order/c.ord")));
  cw_res_get_stats(&s1);
  CHECK_EQ_INT(s1.cached, 0);
  CHECK_EQ_INT(s1.cached_bytes, 0);
  cw_res_release(b2);
  flush_frees();
}

static void test_textures(void) {
  cw_res t = cw_res_load("textures/checker.png", 0);
  t3_texture *tex = cw_res_texture(t);
  CHECK(tex != NULL);
  cw_texture_info ti;
  CHECK(cw_res_texture_info(t, &ti));
  CHECK(ti.width == 64 && ti.height == 64 && !ti.from_ktx2 && !strcmp(ti.format, "rgba8"));
  CHECK(tex && tex->pixels == NULL && tex->_gl != NULL); /* GPU-resident */
  cw_res k = cw_res_load("textures/checker_k.png", 0); /* KTX2 inside (its sidecar) */
  CHECK(cw_res_texture_info(k, &ti));
  CHECK(ti.width == 64 && ti.height == 64 && ti.from_ktx2 && ti.levels == 7);
  CHECK(!strcmp(ti.format, cw_res_ktx2_target()));
  cw_res_release(t);
  cw_res_release(k);
}

static void test_gltf_external_and_lods(void) {
  for (int i = 0; i < CW_RES_FREE_DELAY_FRAMES; i++) cw_res_frame(); /* test_textures' checker.png is gone */
  /* quad.gltf shows checker.png as its base colour: loaded first, it claims
   * the image's texture resource sRGB */
  cw_res q = cw_res_load("models/quad.gltf", 0);
  cw_res tex = cw_res_load("textures/checker.png", 0);
  CHECK(cw_res_texture(tex) && cw_res_texture(tex)->color_space == T3_SRGB_COLOR_SPACE);
  const cw_gltf *g = cw_res_gltf(q);
  CHECK(g != NULL);
  if (!g) return;
  CHECK_EQ_INT(g->mesh_count, 1);
  CHECK_EQ_INT(g->triangles, 2048);
  CHECK_EQ_INT(g->lod_levels, 3);
  FOLD(g->mesh_hash);
  /* the external image is the texture resource itself: shared, not a copy */
  cw_res ts = cw_res_load("models/quad.gltf#texture/0", 0);
  CHECK(cw_res_texture(ts) == cw_res_texture(tex));
  cw_res ms = cw_res_load("models/quad.gltf#mesh/0/0", 0);
  const cw_mesh *m = cw_res_mesh(ms);
  CHECK(m && m->lod_count == 3);
  if (m) {
    CHECK(m->lod_triangles[0] == 2048 && m->lod_triangles[1] < 2048 && m->lod_triangles[2] < m->lod_triangles[1]);
    CHECK(m->lod[1]->attributes[T3_ATTR_POSITION] == m->lod[0]->attributes[T3_ATTR_POSITION]); /* shared vertices */
    FOLD(m->lod_triangles);
  }
  CHECK(cw_res_material(cw_res_load("models/quad.gltf#material/0", 0)) != NULL);
  /* sub-resources are in res.index too: a UID alone finds one */
  cw_res byuid = cw_res_load_uid(cw_res_path_uid("models/quad.gltf#mesh/0/0"), 0);
  CHECK(byuid.id == ms.id);
  cw_res_release(byuid);
  cw_res_release(cw_res_find("models/quad.gltf#material/0"));
  CHECK_EQ_INT(cw_res_ref_count(q), 3); /* ours + two held by the sub-resources */
  /* the same image under a non-default glTF sampler is a texture of its own,
   * sampled the way that glTF says */
  cw_res cs = cw_res_load("models/quad_clamp.gltf#texture/0", 0);
  t3_texture *ct = cw_res_texture(cs);
  CHECK(ct && ct != cw_res_texture(tex) && ct->wrap_s == T3_CLAMP_TO_EDGE && ct->mag_filter == T3_NEAREST);
  cw_texture_info cti;
  CHECK(cw_res_texture_info(cs, &cti) && cti.width == 64);
  CHECK(cw_res_texture(tex)->wrap_s == T3_REPEAT); /* the shared one is untouched */
  cw_res_release(cs);
  /* the same image as data (an occlusion map), while the shared one is sRGB:
   * a texture of its own, with no colour space */
  cw_res ds = cw_res_load("models/quad_data.gltf#texture/0", 0);
  t3_texture *dt = cw_res_texture(ds);
  CHECK(dt && dt != cw_res_texture(tex) && dt->color_space == T3_NO_COLOR_SPACE);
  CHECK(cw_res_texture(tex)->color_space == T3_SRGB_COLOR_SPACE);
  cw_res_release(ds);
  cw_res_release(ts);
  cw_res_release(ms);
  CHECK_EQ_INT(cw_res_ref_count(q), 1);
  cw_res_release(q);
  cw_res_release(tex);
}

static int count_type(t3_object *o, t3_object_type t) {
  int n = o->type == t;
  for (int i = 0; i < o->child_count; i++) n += count_type(o->children[i], t);
  return n;
}
static t3_skinned_mesh *find_skinned(t3_object *o) {
  if (o->type == T3_SKINNED_MESH) return (t3_skinned_mesh *)o;
  for (int i = 0; i < o->child_count; i++) {
    t3_skinned_mesh *s = find_skinned(o->children[i]);
    if (s) return s;
  }
  return NULL;
}
static bool under(t3_object *o, t3_object *root) {
  for (; o; o = o->parent)
    if (o == root) return true;
  return false;
}

static void test_instances(void) {
  cw_res man = cw_res_load_async("models/man.glb", 0, NULL, NULL); /* meshopt-compressed, skinned, LODs */
  CHECK_EQ_INT(cw_res_wait(man), CW_RES_STATE_READY);
  const cw_gltf *g = cw_res_gltf(man);
  CHECK(g && g->lod_levels == 3 && g->triangles == 4672);
  if (!g) return;
  FOLD(g->mesh_hash);
  t3_object *a = cw_gltf_instantiate(man), *b = cw_gltf_instantiate(man), *c = cw_gltf_instantiate(man);
  CHECK_EQ_INT(cw_gltf_instance_count(man), 3);
  CHECK_EQ_INT(cw_res_ref_count(man), 4);
  CHECK(count_type(a, T3_SKINNED_MESH) == 1 && count_type(a, T3_BONE) == count_type(g->gltf->scene, T3_BONE));
  t3_skinned_mesh *sa = find_skinned(a), *sb = find_skinned(b);
  CHECK(sa && sb && sa->mesh.geometry == sb->mesh.geometry); /* shared geometry */
  CHECK(sa && sa->skeleton && under(sa->skeleton->bones[0], a)); /* its own bones */
  int full = cw_gltf_instance_set_lod(b, 0), low = cw_gltf_instance_set_lod(b, 2);
  CHECK(full == 4672 && low < full / 2 + 1);
  CHECK(cw_gltf_instance_set_lod(b, 99) == low); /* clamped */
  FOLD(low);
  /* LOD by screen-space error: near is full detail, far the coarsest, and
   * the choice moves monotonically with distance */
  t3_camera *cam = t3_perspective_camera_new(50, 1, 0.1f, 5000);
  t3_object_set_position(cam, 0, 1, 1.5f);
  t3_object_look_at(cam, 0, 1, 0);
  t3_camera_update_projection_matrix(cam);
  int near_tris = cw_gltf_instance_auto_lod(b, cam, 1080, 0.5f);
  CHECK(near_tris == 4672 && cw_gltf_instance_lod(b) == 0);
  int prev = 0;
  bool mono = true;
  for (int k = 1; k <= 64; k *= 2) {
    t3_object_set_position(cam, 0, 1, 1.5f * (float)k * 20);
    int lv = (cw_gltf_instance_auto_lod(b, cam, 1080, 0.5f), cw_gltf_instance_lod(b));
    mono &= lv >= prev;
    prev = lv;
  }
  CHECK(mono && prev == 2);
  FOLD(prev);
  t3_release(cam);
  t3_release(c);
  CHECK_EQ_INT(cw_gltf_instance_count(man), 2);
  CHECK_EQ_INT(cw_res_ref_count(man), 3);
  t3_release(a);
  t3_release(b);
  CHECK_EQ_INT(cw_res_ref_count(man), 1);
  cw_res_release(man);
}

static void test_sub_async(void) {
  cw_res s = cw_res_load_async("models/box.glb#material/0", 0, NULL, NULL);
  CHECK_EQ_INT(cw_res_get_state(s), CW_RES_STATE_LOADING);
  CHECK_EQ_INT(cw_res_wait(s), CW_RES_STATE_READY);
  CHECK(cw_res_material(s) != NULL);
  CHECK_EQ_INT(cw_res_type_of(s), CW_RES_MATERIAL);
  cw_res_release(s);
}

static void test_reload(void) {
  cw_res n = cw_res_create("gen/note.txt", 0, "one", 3);
  CHECK(cw_res_blob(n) && cw_res_blob(n)->len == 3);
  CHECK(cw_res_reload_from_memory(n, "second", 6));
  CHECK(cw_res_blob(n) && !memcmp(cw_res_blob(n)->data, "second", 6));
  CHECK_EQ_INT(cw_res_version(n), 2);
  CHECK(!cw_res_reload_from_memory(cw_res_create("gen/data.json", 0, "{}", 2), "{bad", 4)); /* old data kept */
  cw_res_release(cw_res_find("gen/data.json"));
  cw_res_release(n);
  /* project.cwproj is key = value text (physics_nodes.h), and a scene is
   * scene text: both load as TEXT, never through the JSON validator */
  cw_res proj = cw_res_create("gen/project.cwproj", 0, "substeps = 4\n", 13);
  CHECK(cw_res_is_ready(proj) && cw_res_type_of(proj) == CW_RES_TEXT);
  cw_res scn = cw_res_create("gen/level.cwscene", 0, "[scene format=1]\n", 17);
  CHECK(cw_res_is_ready(scn) && cw_res_type_of(scn) == CW_RES_TEXT);
  cw_res_release(proj);
  cw_res_release(scn);
  /* textures reload in place: materials holding the t3_texture see it */
  cw_res t = cw_res_load("textures/checker.png", 0);
  t3_texture *before = cw_res_texture(t);
  size_t len;
  void *png = cw_asset_load("textures/small.png", &len);
  CHECK(png && cw_res_reload_from_memory(t, png, len));
  free(png);
  cw_texture_info ti;
  cw_res_texture_info(t, &ti);
  CHECK(cw_res_texture(t) == before && before->width == 16 && ti.width == 16);
  cw_res_release(t);
  /* a glTF reload re-links its instances (the Group stays) */
  cw_res box = cw_res_load("models/box.glb", 0);
  t3_object *inst = cw_gltf_instantiate(box);
  cw_res sub = cw_res_load("models/box.glb#mesh/0/0", 0);
  const cw_mesh *m0 = cw_res_mesh(sub);
  int tri0 = m0 ? m0->lod_triangles[0] : -1;
  void *quad = cw_asset_load("models/quad.gltf", &len);
  CHECK(quad && cw_res_reload_from_memory(box, quad, len));
  free(quad);
  CHECK_EQ_INT(cw_gltf_instance_count(box), 1);
  CHECK(inst->child_count == 1 && count_type(inst, T3_MESH) == 1);
  t3_mesh *mesh = NULL;
  for (int i = 0; i < inst->child_count; i++)
    if (inst->children[i]->type == T3_MESH) mesh = (t3_mesh *)inst->children[i];
  CHECK(mesh && mesh->geometry->index && mesh->geometry->index->count == 2048 * 3);
  CHECK(tri0 == 12 && cw_res_mesh(sub) && cw_res_mesh(sub)->lod_triangles[0] == 2048); /* same handle, new mesh */
  CHECK_EQ_INT(cw_res_version(box), 2);
  t3_release(inst);
  cw_res_release(sub);
  cw_res_release(box);
}

/* cwpack turned a 44.1 kHz stereo WAV into Ogg Opus (its sidecar): the
 * audio module decodes it, at 48 kHz, to the same two tones */
static void test_audio_opus(void) {
  cw_res t = cw_res_load("audio/tune.wav", 0);
  const cw_audio_clip *c = cw_res_audio(t);
  CHECK(c && c->container == CW_AUDIO_OGG_OPUS && c->channels == 2 && c->sample_rate == 48000);
  CHECK(c && c->frames >= 47990 && c->frames <= 48010);
  if (!c) return;
  cw__decoder d;
  CHECK(cw__dec_open(&d, c->data, c->len));
  float *pcm = cw_alloc((size_t)c->frames * 2 * sizeof(float) + 64, CW_MEM_GAME);
  int got = 0, n;
  while (got < (int)c->frames && (n = cw__dec_read(&d, pcm + got * 2, (int)c->frames - got)) > 0) got += n;
  cw__dec_close(&d);
  CHECK(got == (int)c->frames);
  /* signal-to-noise of each channel against its tone, at the best small lag
   * (resampling and the codec delay the signal a little); skip the edges */
  double best[2] = { -1e9, -1e9 };
  for (int ch = 0; ch < 2; ch++) {
    double f = ch ? 660.0 : 440.0;
    for (int lag = -48; lag <= 48; lag++) {
      double sig = 0, err = 0;
      for (int i = 4800; i < got - 4800; i++) {
        double ref = 0.3 * sin(2 * CW_PI * f * (i - lag) / 48000.0);
        double e = pcm[i * 2 + ch] - ref;
        sig += ref * ref;
        err += e * e;
      }
      double snr = 10 * log10(sig / (err + 1e-12));
      if (snr > best[ch]) best[ch] = snr;
    }
  }
  cw_info("opus import: SNR %.1f / %.1f dB", best[0], best[1]);
  CHECK(best[0] >= 20 && best[1] >= 20);
  cw_free(pcm);
  uint32_t bytes = (uint32_t)c->len;
  FOLD(bytes); /* the encoded size: the same bytes on every host */
  cw_res_release(t);
}

/* a level's preload list (cwpack writes it): everything the scene and its
 * sub-scene need, started at once, dependencies before what needs them */
static void test_preload(void) {
  cw_res_group *g = cw_res_group_new();
  int n = cw_res_preload(g, "res://levels/test.cwscene");
  CHECK_EQ_INT(n, 4);
  static const char *want[] = { "models/box.glb", "levels/sub.cwscene", "models/quad.gltf", "levels/test.cwscene" };
  bool order = n == 4;
  for (int i = 0; i < n && i < 4; i++) order &= !strcmp(cw_res_path(cw_res_group_at(g, i)), want[i]);
  CHECK(order);
  cw_res_group_progress p;
  cw_res_group_get_progress(g, &p);
  CHECK(p.total == 4 && p.ready + p.loading + p.failed == 4);
  int frames = 0;
  while (!cw_res_group_done(g) && frames < 1000) { cw_res_frame(); frames++; }
  CHECK(cw_res_group_done(g));
  cw_res_group_get_progress(g, &p);
  CHECK(p.ready == 4 && p.failed == 0 && p.fraction == 1.0f && p.bytes > 0);
  cw_res box = cw_res_find("models/box.glb");
  CHECK_EQ_INT(cw_res_ref_count(box), 1); /* the group's */
  cw_res_group_free(g);
  CHECK_EQ_INT(cw_res_ref_count(box), 0);
  /* a path with no list: the file alone */
  g = cw_res_group_new();
  CHECK_EQ_INT(cw_res_preload(g, "text/hello.txt"), 1);
  cw_res_group_wait(g);
  CHECK(cw_res_is_ready(cw_res_group_at(g, 0)));
  cw_res_group_free(g);
  FOLD(n);
}

static void test_audio_and_stats(void) {
  cw_res a = cw_res_load("audio/beep.wav", 0);
  const cw_audio_clip *c = cw_res_audio(a);
  CHECK(c && c->container == CW_AUDIO_WAV && c->sample_rate == 22050 && c->channels == 1 && c->frames == 5512);
  cw_res_stats s;
  cw_res_get_stats(&s);
  CHECK(s.by_type[CW_RES_AUDIO_CLIP] >= 1 && s.loads > 0 && s.cache_hits > 0); /* tune.wav may wait in the deferred-free queue */
  cw_res_release(a);
  for (int i = 0; i < 4; i++) cw_res_frame();
  cw_res_get_stats(&s);
  CHECK_EQ_INT(s.loading, 0);
  CHECK_EQ_INT(s.pending_free, 0);
}

CW_TEST_SUITE("resource", { "types", test_types }, { "paths_uids", test_paths_and_uids },
              { "deferred_free", test_deferred_free }, { "failures", test_failures },
              { "async_order", test_async_order }, { "priorities", test_priorities },
              { "cancel", test_cancel }, { "residency", test_residency }, { "textures", test_textures },
              { "gltf_external_lods", test_gltf_external_and_lods }, { "instances", test_instances },
              { "sub_async", test_sub_async }, { "reload", test_reload }, { "audio_opus", test_audio_opus }, { "preload", test_preload }, { "audio_stats", test_audio_and_stats })

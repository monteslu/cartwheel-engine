/* cartwheel/terrain.h unit suite:
 *   height-exact     cw_terrain_height_at equals a downward cw_world3d ray
 *                    cast against the terrain's height fields at 10 000
 *                    random points, bit for bit, and the triangles' own
 *                    height to a millimetre;
 *   cdlod            from several cameras, neighbouring patches differ by
 *                    one level at most, a finer patch is fully morphed
 *                    where it meets a coarser one and the coarser one is
 *                    not morphing there (the shader's formula, run here):
 *                    no cracks; the selection covers the terrain exactly
 *                    once;
 *   physics          balls rolling down generated terrain give the same
 *                    bits with one step worker and with all of them;
 *   vehicle          a cw_vehicle3d car on the terrain settles on four
 *                    wheels and drives up a slope; a cw_mover3d walks on it;
 *   scatter          placement is the same for the same seed, differs for
 *                    another, respects density, slope and layers;
 *   import / control Terrain3D control words round trip; a 16-bit PNG and the
 *                    same heights as .r16 import exactly; a splat image picks
 *                    the two heaviest layers; a control PNG gives its words;
 *   scene file       a Terrain3D with a TerrainScatter3D child from .cwscene
 *                    text: imported heights, physics in the tree's world,
 *                    the scatter layer, the properties saved back, and the
 *                    layer gone with its node.
 * Every result folds into unit.checksum, compared across hosts. */
#include <math.h>
#include <string.h>

#include "cartwheel/physics_nodes.h"
#include "cartwheel/scene_file.h"
#include "cartwheel/terrain.h"
#include "cartwheel/vehicle3d.h"
#include "terrain/terrain_internal.h"
#include "test.h"

static cw_terrain *make(int size, uint32_t seed) {
  cw_terrain_def d = cw_terrain_default_def();
  d.size_x = d.size_z = size;
  d.region_size = 128;
  d.vertex_spacing = 1;
  d.min_height = -10;
  d.max_height = 70;
  d.origin = (t3_vec3){ -(float)size / 2, 0, -(float)size / 2 };
  cw_terrain *t = cw_terrain_create(&d);
  cw_terrain_generate(t, seed, 120, 6, 0.9f);
  return t;
}

static cw_world3d world(int workers) {
  cw_world3d_def wd = cw_world3d_default_def();
  wd.workers = workers;
  wd.sleep = false;
  return cw_world3d_create(&wd);
}

static uint32_t rnd_state = 12345;
static float frand(void) {
  rnd_state = rnd_state * 1664525u + 1013904223u;
  return (float)(rnd_state >> 8) / 16777216.0f;
}

static void test_height_exact(void) {
  cw_terrain *t = make(256, 7);
  cw_world3d w = world(0);
  cw_terrain_attach_physics(t, w, NULL);
  const cw_terrain_def *d = cw_terrain_get_def(t);
  float top = d->origin.y + d->max_height + 1.0f, len = d->max_height - d->min_height + 2.0f;
  int exact = 0, near = 0, hits = 0;
  float worst = 0;
  rnd_state = 99;
  for (int i = 0; i < 10000; i++) {
    float x = d->origin.x + frand() * 256, z = d->origin.z + frand() * 256;
    float h = cw_terrain_height_at(t, x, z);
    cw_ray_hit3d hit;
    if (cw_world3d_raycast(w, (t3_vec3){ x, top, z }, (t3_vec3){ 0, -len, 0 }, NULL, &hit)) {
      hits++;
      exact += memcmp(&hit.point.y, &h, sizeof h) == 0;
    }
    float tri = cw__terrain_height_fast(t, x - d->origin.x, z - d->origin.z);
    float e = fabsf(tri - h);
    worst = e > worst ? e : worst;
    near += e < 1e-3f;
    FOLD(h);
  }
  CHECK_EQ_INT(hits, 10000);
  CHECK_EQ_INT(exact, 10000);
  CHECK_EQ_INT(near, 10000);
  if (exact != 10000 || near != 10000) cw_info("terrain height: %d exact, %d near, worst %g m", exact, near, worst);
  /* a ray from the side and one straight down agree */
  cw_terrain_hit th;
  CHECK(cw_terrain_raycast(t, (t3_vec3){ 3.3f, 100, -7.7f }, (t3_vec3){ 0, -1, 0 }, 200, &th));
  CHECK(fabsf(th.point.y - cw_terrain_height_at(t, 3.3f, -7.7f)) < 1e-4f);
  FOLD(th.point);
  /* a hole lets rays through */
  uint32_t c = cw_terrain_control(1, 0, 0, 0, 0, true);
  uint32_t holes[9];
  for (int i = 0; i < 9; i++) holes[i] = c;
  cw_terrain_set_control(t, 127, 127, 3, 3, holes);
  cw_terrain_sync_physics(t);
  cw_ray_hit3d hit;
  bool through = !cw_world3d_raycast(w, (t3_vec3){ 0.5f, top, 0.5f }, (t3_vec3){ 0, -len, 0 }, NULL, &hit);
  CHECK(through);
  CHECK(cw_terrain_is_hole(t, 0.5f, 0.5f));
  cw_terrain_destroy(t);
  cw_world3d_destroy(w);
}

/* the vertex stage's morph (terrain_shading.mjs), on the CPU */
static float morph_k(const cw_terrain *t, int lod, int tx, int tz, t3_vec3 cam) {
  const cw_terrain_def *d = &t->def;
  float wx = d->origin.x + (float)tx * d->vertex_spacing, wz = d->origin.z + (float)tz * d->vertex_spacing;
  float y = tr_height_m(t, t->h[tr_idx(t, tx, tz)]);
  float dx = wx - cam.x, dy = y - cam.y, dz = wz - cam.z;
  float dist = sqrtf(dx * dx + dy * dy + dz * dz);
  float range = t->range[0] * exp2f((float)lod * log2f(d->lod_ratio));
  float prev = lod > 0 ? range / d->lod_ratio : 0;
  float end = range * 0.985f, start = prev + (end - prev) * d->morph_start;
  float k = (dist - start) / (end - start);
  return k < 0 ? 0 : k > 1 ? 1 : k;
}

/* the crack audit over the selections from four cameras; shrink < 1 scales the ranges past the
 * crack-free bound (the control: the audit must then find cracks) */
static void audit(cw_terrain *t, float shrink, int *bad_levels_out, int *bad_fine_out, int *bad_coarse_out,
                  int *bad_cover_out, int *checked_out) {
  const t3_vec3 cams[] = { { 0, 60, 0 }, { -300, 45, 210 }, { 480, 120, -470 }, { 10, 5, 400 } };
  int bad_levels = 0, bad_fine = 0, bad_coarse = 0, bad_cover = 0, checked = 0;
  uint8_t *cover = cw_alloc((size_t)1024 * 1024, CW_MEM_GAME);
  for (int ci = 0; ci < (int)CW_ARRAY_LEN(cams); ci++) {
    t3_vec3 cam = cams[ci];
    cw_terrain_update(t, cam, NULL);
    if (shrink < 1) {
      /* the control: levels reaching less far than the bound, the selection redone */
      t->range[0] *= shrink;
      for (int l = 1; l < t->lods; l++) t->range[l] = t->range[l - 1] * t->def.lod_ratio;
      t->def.lod0_range = t->range[0];
      cw__terrain_select(t, cam, NULL);
    }
    cw_terrain_stats st;
    cw_terrain_get_stats(t, &st);
    FOLD(st.selection_hash);
    int n = t->patches.count;
    const tr_patch *p = t->patches.data;
    /* every quad covered exactly once */
    memset(cover, 0, (size_t)1024 * 1024);
    for (int i = 0; i < n; i++)
      for (int z = p[i].z; z < p[i].z + p[i].size; z++)
        for (int x = p[i].x; x < p[i].x + p[i].size; x++) cover[z * 1024 + x]++;
    for (int i = 0; i < 1024 * 1024; i++) bad_cover += cover[i] != 1;
    for (int i = 0; i < n; i++)
      for (int j = 0; j < n; j++) {
        const tr_patch *a = &p[i], *b = &p[j];
        if (a->lod >= b->lod) continue; /* a finer */
        /* a shared edge segment */
        int x0 = CW_MAX(a->x, b->x), x1 = CW_MIN(a->x + a->size, b->x + b->size);
        int z0 = CW_MAX(a->z, b->z), z1 = CW_MIN(a->z + a->size, b->z + b->size);
        bool vert = (a->x + a->size == b->x || b->x + b->size == a->x) && z1 > z0;
        bool horz = (a->z + a->size == b->z || b->z + b->size == a->z) && x1 > x0;
        if (!vert && !horz) continue;
        if (b->lod - a->lod > 1) bad_levels++;
        int ex = vert ? (a->x + a->size == b->x ? b->x : a->x) : 0, ez = horz ? (a->z + a->size == b->z ? b->z : a->z) : 0;
        int fs = 1 << a->lod, cs = 1 << b->lod;
        for (int s = vert ? z0 : x0; s <= (vert ? z1 : x1); s += fs) {
          int tx = vert ? ex : s, tz = vert ? s : ez;
          checked++;
          if (morph_k(t, a->lod, tx, tz, cam) < 1) bad_fine++;
          if ((s / cs) * cs == s) continue; /* even at the coarse level: it does not move */
          (void)cs;
        }
        for (int s = vert ? z0 : x0; s <= (vert ? z1 : x1); s += cs) {
          int tx = vert ? ex : s, tz = vert ? s : ez;
          if (morph_k(t, b->lod, tx, tz, cam) > 0) bad_coarse++;
        }
      }
  }
  cw_free(cover);
  *bad_levels_out = bad_levels, *bad_fine_out = bad_fine, *bad_coarse_out = bad_coarse;
  *bad_cover_out = bad_cover, *checked_out = checked;
}

static void test_cdlod(void) {
  cw_terrain *t = make(1024, 3);
  int bad_levels, bad_fine, bad_coarse, bad_cover, checked;
  audit(t, 1, &bad_levels, &bad_fine, &bad_coarse, &bad_cover, &checked);
  CHECK_EQ_INT(bad_cover, 0);
  CHECK_EQ_INT(bad_levels, 0);
  CHECK_EQ_INT(bad_fine, 0);
  CHECK_EQ_INT(bad_coarse, 0);
  CHECK(checked > 100);
  /* the control: ranges a third of the bound must show cracks to the same audit */
  {
    cw_terrain *u = make(1024, 3);
    int cl, cf, cc, cv, ck;
    audit(u, 0.33f, &cl, &cf, &cc, &cv, &ck);
    CHECK(cl + cf + cc > 0);
    CHECK_EQ_INT(cv, 0); /* (still covered once: the cracks are between levels) */
    cw_terrain_destroy(u);
  }
  /* culling: the frustum keeps fewer patches */
  t3_camera *c = t3_perspective_camera_new(60, 16.0f / 9, 0.1f, 5000);
  c->base.position = (t3_vec3){ 0, 60, 0 };
  t3_object_look_at(c, 100, 20, 100);
  t3_object_update_matrix_world(c, true);
  t3_camera_update_projection_matrix(c);
  t3_mat4 vp;
  t3_mat4_multiply(&vp, &c->projection_matrix, &c->matrix_world_inverse);
  cw_terrain_update(t, (t3_vec3){ 0, 60, 0 }, NULL);
  int all = t->patches.count;
  cw_terrain_update(t, (t3_vec3){ 0, 60, 0 }, &vp);
  CHECK(t->patches.count < all && t->patches.count > 0);
  t3_release(c);
  cw_terrain_destroy(t);
}

static uint32_t roll(int workers) {
  cw_terrain *t = make(256, 11);
  cw_world3d w = world(workers);
  cw_terrain_attach_physics(t, w, NULL);
  cw_body3d balls[24];
  for (int i = 0; i < 24; i++) {
    cw_body3d_def bd = cw_body3d_default_def();
    float x = -90 + (float)(i % 6) * 36, z = -90 + (float)(i / 6) * 50;
    bd.position = (t3_vec3){ x, cw_terrain_height_at(t, x, z) + 2, z };
    bd.sleep = false;
    balls[i] = cw_body3d_create(w, &bd);
    cw_shape3d_def sd = cw_shape3d_default_def();
    sd.material.rolling_resistance = 0.02f;
    cw_shape3d_sphere(balls[i], &sd, (t3_vec3){ 0, 0, 0 }, 0.5f + 0.05f * (float)(i % 5));
  }
  for (int s = 0; s < 300; s++) cw_world3d_step(w, 1.0f / 60);
  uint32_t h = CW_HASH32_INIT;
  int below = 0;
  for (int i = 0; i < 24; i++) {
    cw_xform3 x = cw_body3d_transform(balls[i]);
    h = cw_hash32(&x, sizeof x, h);
    below += x.p.y < cw_terrain_height_at(t, x.p.x, x.p.z) - 0.1f;
  }
  CHECK_EQ_INT(below, 0);
  cw_terrain_destroy(t);
  cw_world3d_destroy(w);
  return h;
}

static void test_physics(void) {
  uint32_t a = roll(1), b = roll(0);
  CHECK_EQ_INT(a, b);
  FOLD(a);
}

static void test_vehicle(void) {
  /* rolling country: 6 m of relief over 120 m features */
  cw_terrain *t = make(256, 5);
  cw_terrain_generate(t, 5, 120, 5, 0.075f);
  cw_world3d w = world(0);
  cw_terrain_attach_physics(t, w, NULL);
  cw_body3d_def bd = cw_body3d_default_def();
  bd.position = (t3_vec3){ 0, cw_terrain_height_at(t, 0, 0) + 1.5f, 0 };
  bd.sleep = false;
  cw_body3d chassis = cw_body3d_create(w, &bd);
  cw_shape3d_def sd = cw_shape3d_default_def();
  sd.density = 1200.0f / (1.8f * 0.7f * 4.4f);
  cw_shape3d_box(chassis, &sd, (t3_vec3){ 0.9f, 0.35f, 2.2f }, cw_xform3_identity());
  cw_vehicle3d_def vd = cw_vehicle3d_default_def();
  cw_vehicle3d_def_add_wheel(&vd, (t3_vec3){ -0.85f, -0.2f, 1.4f }, 0);
  cw_vehicle3d_def_add_wheel(&vd, (t3_vec3){ 0.85f, -0.2f, 1.4f }, 0);
  cw_vehicle3d_def_add_wheel(&vd, (t3_vec3){ -0.85f, -0.2f, -1.4f }, 1);
  cw_vehicle3d_def_add_wheel(&vd, (t3_vec3){ 0.85f, -0.2f, -1.4f }, 1);
  cw_vehicle3d car;
  cw_vehicle3d_init(&car, chassis, &vd);
  for (int s = 0; s < 120; s++) {
    cw_vehicle3d_update(&car, 1.0f / 60);
    cw_world3d_step(w, 1.0f / 60);
  }
  CHECK_EQ_INT(car.contacts, 4);
  t3_vec3 p0 = cw_body3d_transform(chassis).p;
  cw_vehicle3d_set_axle(&car, 1, 0, 6000, 0);
  for (int s = 0; s < 180; s++) {
    cw_vehicle3d_update(&car, 1.0f / 60);
    cw_world3d_step(w, 1.0f / 60);
  }
  t3_vec3 p1 = cw_body3d_transform(chassis).p;
  float moved = sqrtf((p1.x - p0.x) * (p1.x - p0.x) + (p1.z - p0.z) * (p1.z - p0.z));
  CHECK(moved > 5);
  CHECK(p1.y > cw_terrain_height_at(t, p1.x, p1.z));
  FOLD(p1);
  /* a character walks over it and stays on the ground */
  cw_mover3d m;
  cw_mover3d_init(&m, w, NULL, (t3_vec3){ 20, cw_terrain_height_at(t, 20, 20) + 0.05f, 20 });
  int floor = 0;
  for (int s = 0; s < 240; s++) {
    m.velocity = (t3_vec3){ 3, m.velocity.y - 9.8f / 60, 0 };
    cw_mover3d_move(&m, 1.0f / 60);
    floor += m.on_floor;
  }
  CHECK(floor > 200);
  CHECK(fabsf(m.position.y - cw_terrain_height_at(t, m.position.x, m.position.z)) < 0.35f);
  FOLD(m.position);
  cw_terrain_destroy(t);
  cw_world3d_destroy(w);
}

static void test_scatter(void) {
  cw_terrain *t = make(256, 9);
  /* layer 1 on the eastern half */
  uint32_t *ctl = cw_alloc(sizeof(uint32_t) * 257 * 257, CW_MEM_GAME);
  for (int z = 0; z <= 256; z++)
    for (int x = 0; x <= 256; x++) ctl[z * 257 + x] = cw_terrain_control(x >= 128 ? 1 : 0, 0, 0, 0, 0, false);
  cw_terrain_set_control(t, 0, 0, 257, 257, ctl);
  cw_free(ctl);
  t3_geometry *g = t3_box_geometry_new(1, 1, 1, 1, 1, 1);
  t3_material *m = t3_mesh_standard_material_new(0x808080);
  cw_scatter_def d = cw_scatter_default_def();
  d.geometry[0][0] = g;
  d.material[0][0] = m;
  d.density = 1;
  d.seed = 42;
  int a = cw_terrain_scatter_add(t, &d);
  d.seed = 43;
  int b = cw_terrain_scatter_add(t, &d);
  d.seed = 42;
  d.layers = 1u << 1;
  d.min_slope_cos = 0.95f;
  int c = cw_terrain_scatter_add(t, &d);
  static float pa[5 * 8192], pb[5 * 8192], pc[5 * 8192];
  int total = 0, same = 0, diff = 0, wrong_layer = 0, steep = 0;
  for (int cz = 0; cz < 8; cz++)
    for (int cx = 0; cx < 8; cx++) {
      int na = cw_terrain_scatter_cell(t, a, cx, cz, pa, 8192);
      int na2 = cw_terrain_scatter_cell(t, a, cx, cz, pb, 8192);
      same += na == na2 && !memcmp(pa, pb, sizeof(float) * 5 * (size_t)na);
      int nb = cw_terrain_scatter_cell(t, b, cx, cz, pb, 8192);
      diff += nb != na || memcmp(pa, pb, sizeof(float) * 5 * (size_t)CW_MIN(na, nb)) != 0;
      total += na;
      FOLD(na);
      if (na) cw_test_checksum = cw_hash32(pa, sizeof(float) * 5 * (size_t)na, cw_test_checksum);
      int nc = cw_terrain_scatter_cell(t, c, cx, cz, pc, 8192);
      for (int i = 0; i < nc; i++) {
        wrong_layer += pc[5 * i] - t->def.origin.x < 127.5f;
        steep += cw_terrain_normal_at(t, pc[5 * i], pc[5 * i + 2]).y < 0.95f - 1e-4f;
      }
    }
  CHECK_EQ_INT(same, 64);
  CHECK_EQ_INT(diff, 64);
  /* 1 per square metre over 256^2 m, give or take the strata's chance */
  CHECK(total > 256 * 256 * 9 / 10 && total < 256 * 256 * 11 / 10);
  CHECK_EQ_INT(wrong_layer, 0);
  CHECK_EQ_INT(steep, 0);
  t3_release(g);
  t3_release(m);
  cw_terrain_destroy(t);
}

static void test_control(void) {
  uint32_t c = cw_terrain_control(5, 17, 200, 9, 3, true);
  CHECK_EQ_INT(c >> 27, 5);
  CHECK_EQ_INT((c >> 22) & 31, 17);
  CHECK_EQ_INT((c >> 14) & 255, 200);
  CHECK_EQ_INT((c >> 10) & 15, 9);
  CHECK_EQ_INT((c >> 7) & 7, 3);
  CHECK_EQ_INT((c >> 2) & 1, 1);
  FOLD(c);
  /* quantization: metres to steps as Box3D does */
  cw_terrain *t = make(64, 1);
  float m[4] = { -10, 70, 30, 1000 };
  cw_terrain_set_heights_m(t, 0, 0, 4, 1, m);
  const uint16_t *h = cw_terrain_heights(t);
  CHECK_EQ_INT(h[0], 0);
  CHECK_EQ_INT(h[1], 65535);
  CHECK_EQ_INT(h[3], 65535);
  CHECK(fabsf(tr_height_m(t, h[2]) - 30) < 0.002f);
  FOLD(h[2]);
  cw_terrain_destroy(t);
}


/* the fixtures (terrain_assets/, written by a script: see its README) are 65 x 65 */
static uint16_t fixture_h(int x, int y) { return (uint16_t)((x * 1009 + y * 523 + ((x * y) % 97) * 13) % 65536); }

static void test_import(void) {
  cw_terrain_def d = cw_terrain_default_def();
  d.size_x = d.size_z = 64;
  d.grid_quads = 32;
  d.region_size = 64;
  int bad = 0;
  for (int k = 0; k < 2; k++) {
    cw_terrain *t = cw_terrain_create(&d);
    CHECK(cw_terrain_import_heightmap(t, k ? "heights.r16" : "heights.png"));
    const uint16_t *h = cw_terrain_heights(t);
    for (int y = 0; y < 65; y++)
      for (int x = 0; x < 65; x++) bad += h[y * 65 + x] != fixture_h(x, y);
    cw_terrain_destroy(t);
  }
  CHECK_EQ_INT(bad, 0);
  cw_terrain *t = cw_terrain_create(&d);
  /* splat: R = 3x (layer 0), G = 3y (layer 1), B = 250 within 4 of x = 32 (layer 2), A = 0 */
  CHECK(cw_terrain_import_splat(t, "splat.png", 0));
  const uint32_t *c = cw_terrain_controls(t);
  uint32_t a = c[10 * 65 + 60], b = c[50 * 65 + 5], m = c[20 * 65 + 32];
  CHECK_EQ_INT(a >> 27, 0);           /* x 60 (R 180) over y 10 (G 30) */
  CHECK_EQ_INT((a >> 22) & 31, 1);
  CHECK_EQ_INT(b >> 27, 1);           /* y 50 (G 150) over x 5 (R 15) */
  CHECK_EQ_INT(m >> 27, 2);           /* the band */
  CHECK_EQ_INT((m >> 14) & 255, 96 * 255 / (250 + 96)); /* blend = the second's share (R 96 at x 32) */
  FOLD(a), FOLD(b), FOLD(m);
  /* control: (x & 7) << 27 | (y & 7) << 22 | 100 << 14 */
  CHECK(cw_terrain_import_control(t, "control.png"));
  c = cw_terrain_controls(t);
  int badc = 0;
  for (int y = 0; y < 65; y++)
    for (int x = 0; x < 65; x++) badc += c[y * 65 + x] != (((uint32_t)(x & 7) << 27) | ((uint32_t)(y & 7) << 22) | (100u << 14));
  CHECK_EQ_INT(badc, 0);
  cw_terrain_destroy(t);
}

static const char scene_text[] =
  "[scene format=1]\n\n"
  "[node name=\"Root\" type=\"Node3D\"]\n\n"
  "[node name=\"Ground\" parent=\".\" type=\"Terrain3D\"]\n"
  "position = vec3(100, 5, -50)\n"
  "size_x = 64\nsize_z = 64\nregion_size = 32\n"
  "min_height = 0\nmax_height = 40\n"
  "heightmap = \"heights.r16\"\ncontrol = \"control.png\"\n"
  "layer1_uv_scale = 0.5\n\n"
  "[node name=\"Stones\" parent=\"Ground\" type=\"TerrainScatter3D\"]\n"
  "shape = ROCK\ndensity = 0.05\nseed = 7\n";

static void test_scene_file(void) {
  cw_packed_scene *ps = cw_scene_parse(scene_text, "terrain_test.cwscene");
  CHECK(ps != NULL);
  if (!ps) return;
  cw_tree *tree = cw_tree_new();
  cw_node root = cw_scene_instance(ps);
  cw_node_add_child(cw_tree_root(tree), root);
  cw_tree_fixed_update(tree, 1.0 / 60);
  cw_node ground = cw_node_get(root, "Ground");
  cw_terrain *t = cw_terrain_of(ground);
  CHECK(t != NULL);
  if (t) {
    const cw_terrain_def *d = cw_terrain_get_def(t);
    /* centred on the node */
    CHECK(d->origin.x == 100 - 32 && d->origin.z == -50 - 32 && d->origin.y == 5);
    CHECK_EQ_INT(cw_terrain_heights(t)[3 * 65 + 7], fixture_h(7, 3));
    CHECK_EQ_INT(cw_terrain_controls(t)[2 * 65 + 9], ((9u & 7) << 27) | ((2u & 7) << 22) | (100u << 14));
    /* physics in the tree's world: a ray straight down hits the sample's height */
    float x = d->origin.x + 7, z = d->origin.z + 3;
    cw_ray_hit3d hit;
    CHECK(cw_world3d_raycast(cw_physics3d_world(tree), (t3_vec3){ x, 100, z }, (t3_vec3){ 0, -200, 0 }, NULL, &hit));
    CHECK(fabsf(hit.point.y - cw_terrain_height_at(t, x, z)) < 1e-4f);
    CHECK(t->scatter[0] != NULL); /* the Stones layer */
    FOLD(hit.point.y);
  }
  char *text = cw_scene_save(root);
  CHECK(text && strstr(text, "heightmap = \"heights.r16\"") && strstr(text, "type=\"TerrainScatter3D\"") &&
        strstr(text, "layer1_uv_scale = 0.5"));
  cw_free(text);
  /* a scatter node that leaves takes its layer with it */
  cw_node_free(cw_node_get(root, "Ground/Stones"));
  cw_tree_fixed_update(tree, 1.0 / 60);
  CHECK(t && t->scatter[0] == NULL);
  cw_tree_free(tree);
}

CW_TEST_SUITE("terrain", { "height_exact", test_height_exact }, { "cdlod", test_cdlod }, { "physics", test_physics },
              { "vehicle", test_vehicle }, { "scatter", test_scatter }, { "control", test_control },
              { "import", test_import }, { "scene_file", test_scene_file })

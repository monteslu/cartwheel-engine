/* vis_city: a procedural city of about 4500 meshes under a
 * VisibilityManager: 16 x 16 blocks of buildings (shared unit boxes in six
 * colours: they instance), street lamps with a visibility_range, generated
 * glTF trees with cwpack LOD chains and impostors past 80 m, and three hollow
 * houses with furnished rooms joined by Portal3D doorways. The camera walks
 * a fixed path: down a long street (buildings occlude most of the city), into
 * a house, through an inner doorway, around and back out. The street run
 * is fast (up to 0.9 m a frame) and brushes the street trees, so the
 * occlusion guard (visibility.h, occlusion_guard) issues no queries from
 * about tick 10 to 220: city.queries and city.occluded read 0 there and
 * fill in as the camera slows toward the house (tick 240: 391 queries, 160
 * occluded). Frustum, distance and portal culling run throughout.
 *
 * VIS_MODE picks what the manager does:
 *   full   (default) frustum (BVH), occlusion queries, portals, LOD, impostors
 *   cull   frustum, occlusion and portals only (no LOD, impostors or
 *          visibility ranges): every frame looks like `off`
 *   off    no VisibilityManager at all
 *   check  `cull` in this tree next to a second tree holding the same city
 *          with no culling at all, both drawn every frame and compared:
 *          city.max_pop is the most pixels (of 960x540) that differ by more
 *          than 24 levels in one frame, city.pop_frames the frames with more
 *          than 64 such pixels (a missing building or tree; instance order
 *          flips a few pixels of coplanar faces), city.pop_log the first 16
 *          frames over 4 (tick * 100000 + pixels). city.pop_free =
 *          pop_frames <= 1 and max_pop < 1000: one isolated small frame is
 *          tolerated (Chromium shows one such frame in a few runs; the naive
 *          control shows a dozen, up to 97000 pixels)
 *   naive  `check` with occlusion_guard off: plain one-frame-late queries,
 *          which pop when the camera turns or passes doorways (the gate's
 *          control)
 * VIS_NO_OCC / VIS_NO_PORTALS / VIS_NO_FRUSTUM switch one culler off (to
 * bisect a pop); VIS_LOG prints the frames that differ; natively
 * VIS_DUMP=<tick> writes both images to out/vis_a.ppm and out/vis_b.ppm.
 * Occlusion results are read with occlusion_wait (the same counts on every
 * host).
 *
 * Debug state: city.meshes, city.visible, city.frustum, city.occluded,
 * city.portal, city.distance, city.queries, city.lod (4 counts),
 * city.impostors, city.rooms_visible, city.draws (three.c draw calls),
 * city.tick, and timings (reported, never compared): city.frame_ms,
 * city.cpu_ms (update start to a GPU sync after the frame), city.cull_ms,
 * city.draws_avg, city.visible_avg (after 60 warm-up frames), city.tris_k,
 * city.gpu_ms (the pipeline's timed passes). The vis_city_check and
 * vis_city_off builds default to VIS_MODE=check and off. */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "cartwheel/debug.h"
#include "cartwheel/gl.h"
#include "cartwheel/pipeline.h"
#include "cartwheel/platform.h"
#include "cartwheel/render3d.h"
#include "cartwheel/resource.h"
#include "cartwheel/scene_file.h"
#include "cartwheel/visibility.h"

#define BLOCKS 16
#define BLOCK 30.0f
#define STREET 10.0f
#define WARMUP 60
#ifndef VIS_DEFAULT_MODE
#define VIS_DEFAULT_MODE "full"
#endif

static cw_tree *tree;
static cw_node world, cam, mgr;
static cw_rng rng;
static uint32_t tick;
static int32_t meshes, visible, frustum, occluded, portal, distance, queries, lod[4], impostors, rooms_vis, draws;
static float frame_ms, cpu_ms, cull_ms, draws_avg, visible_avg, tris_k, gpu_ms;
static double sum_draws, sum_visible;
static double t_update, t_last, sum_frame, sum_cpu;
static int timed;
static bool check, no_ranges;
static cw_tree *ref_tree;
static cw_node ref_cam;
static cw_rt *ref_rt;
static uint8_t *img_a, *img_b;
static int32_t bvh_nodes, static_meshes, max_pop, pop_free = 1, max_pop_tick, late_total, pop_log[16], pop_logged, pop_frames;

static double now_ms(void) {
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (double)ts.tv_sec * 1e3 + (double)ts.tv_nsec / 1e6;
}

/* the palette is picked as screen (sRGB) colours; materials take linear ones */
static float lin(float c) { return c <= 0.04045f ? c / 12.92f : powf((c + 0.055f) / 1.055f, 2.4f); }

static cw_node box(cw_node parent, float x, float y, float z, float sx, float sy, float sz, const float *rgb) {
  cw_node n = cw_node_new("MeshInstance3D", "B");
  cw_node3d_set_position(n, (t3_vec3){ x, y, z });
  cw_node_set(n, "size", cw_val_vec3(sx, sy, sz));
  cw_node_set(n, "color", cw_val_color(lin(rgb[0]), lin(rgb[1]), lin(rgb[2]), 1));
  cw_node_set(n, "static", cw_val_bool(true));
  cw_node_add_child(parent, n);
  return n;
}

static cw_node prim(cw_node parent, const char *kind, float x, float y, float z, float sx, float sy, const float *rgb) {
  cw_node n = cw_node_new("MeshInstance3D", "P");
  cw_node_set(n, "primitive", cw_val_enum(kind));
  cw_node3d_set_position(n, (t3_vec3){ x, y, z });
  cw_node_set(n, "size", cw_val_vec3(sx, sy, sx));
  cw_node_set(n, "color", cw_val_color(lin(rgb[0]), lin(rgb[1]), lin(rgb[2]), 1));
  cw_node_set(n, "static", cw_val_bool(true));
  cw_node_add_child(parent, n);
  return n;
}

static const float palette[6][3] = { { 0.62f, 0.58f, 0.52f }, { 0.45f, 0.47f, 0.52f }, { 0.7f, 0.66f, 0.6f },
                                     { 0.55f, 0.42f, 0.36f }, { 0.38f, 0.4f, 0.38f }, { 0.75f, 0.72f, 0.68f } };
static const float wall_c[3] = { 0.8f, 0.78f, 0.72f }, roof_c[3] = { 0.45f, 0.3f, 0.25f };

/* a hollow house over [x0, x0 + 20] x [z0, z0 + 20]: west door, an inner
 * wall at x0 + 10 with a doorway, two furnished rooms */
static void house(float x0, float z0) {
  const float H = 4, T = 0.3f, D = 1.6f, DH = 2.4f, zc = z0 + 10;
  cw_node h = cw_node_new("Node3D", "House");
  cw_node_add_child(world, h);
  /* west wall with the door at zc */
  box(h, x0, H / 2, (z0 + zc - D / 2) / 2, T, H, zc - D / 2 - z0, wall_c);
  box(h, x0, H / 2, (zc + D / 2 + z0 + 20) / 2, T, H, z0 + 20 - zc - D / 2, wall_c);
  box(h, x0, (DH + H) / 2, zc, T, H - DH, D, wall_c);
  /* east, north, south */
  box(h, x0 + 20, H / 2, zc, T, H, 20, wall_c);
  box(h, x0 + 10, H / 2, z0, 20, H, T, wall_c);
  box(h, x0 + 10, H / 2, z0 + 20, 20, H, T, wall_c);
  /* inner wall with a doorway, offset */
  float zd = zc + 4;
  box(h, x0 + 10, H / 2, (z0 + zd - D / 2) / 2, T, H, zd - D / 2 - z0, wall_c);
  box(h, x0 + 10, H / 2, (zd + D / 2 + z0 + 20) / 2, T, H, z0 + 20 - zd - D / 2, wall_c);
  box(h, x0 + 10, (DH + H) / 2, zd, T, H - DH, D, wall_c);
  box(h, x0 + 10, H + 0.15f, zc, 20.6f, 0.3f, 20.6f, roof_c);
  for (int r = 0; r < 2; r++) {
    cw_node room = cw_node_new("Room3D", "Room");
    cw_node3d_set_position(room, (t3_vec3){ x0 + 5 + 10 * r, H / 2, zc });
    cw_node_set(room, "extents", cw_val_vec3(4.9f, H / 2, 9.9f));
    cw_node_add_child(h, room);
    for (int i = 0; i < 40; i++) {
      float fx = x0 + 10 * r + cw_rng_range(&rng, 1, 9), fz = z0 + cw_rng_range(&rng, 1, 19);
      if (fz > zc - 2.5f && fz < zd + 2.5f) continue; /* keep the walk between the doors clear */
      /* one draw per statement: C leaves the order of arguments and initializers open */
      float c[3];
      for (int q = 0; q < 3; q++) c[q] = cw_rng_range(&rng, 0.3f, 0.9f);
      int k = cw_rng_int(&rng, 0, 2);
      cw_node f;
      float sx = cw_rng_range(&rng, 0.4f, 1.4f);
      float sz = cw_rng_range(&rng, 0.4f, 1.4f);
      if (k == 0) f = box(h, fx, 0.4f, fz, sx, 0.8f, sz, c);
      else if (k == 1) f = prim(h, "SPHERE", fx, 0.5f, fz, 0.9f, 0.9f, c);
      else f = prim(h, "CYLINDER", fx, 0.6f, fz, 0.6f, 1.2f, c);
      if (!no_ranges) cw_node_set(f, "visibility_range", cw_val_float(60));
    }
  }
  /* the doorways */
  float pz[2] = { zc, zd }, px[2] = { x0, x0 + 10 };
  for (int i = 0; i < 2; i++) {
    cw_node p = cw_node_new("Portal3D", "Door");
    cw_node3d_set_position(p, (t3_vec3){ px[i], DH / 2, pz[i] });
    cw_node3d_set_rotation(p, (t3_vec3){ 0, CW_PI / 2, 0 });
    cw_node_set(p, "size", cw_val_vec2(D, DH));
    cw_node_add_child(h, p);
  }
}

static bool is_house_block(int bx, int bz) {
  return (bx == 8 && bz == 9) || (bx == 3 && bz == 4) || (bx == 12 && bz == 6);
}

static void build_city(void) {
  cw_rng_seed(&rng, 2026);
  uint64_t tree_uid = cw_res_path_uid("models/tree.glb");
  for (int bz = 0; bz < BLOCKS; bz++)
    for (int bx = 0; bx < BLOCKS; bx++) {
      float x0 = -BLOCKS * BLOCK / 2 + bx * BLOCK + STREET / 2, z0 = -BLOCKS * BLOCK / 2 + bz * BLOCK + STREET / 2;
      float inner = BLOCK - STREET;
      if (is_house_block(bx, bz)) house(x0, z0);
      else {
        /* 2 x 2 or 3 x 2 lots of buildings */
        int nx = cw_rng_int(&rng, 2, 3), nz = 2;
        for (int j = 0; j < nz; j++)
          for (int i = 0; i < nx; i++) {
            float w = inner / nx, d = inner / nz;
            float hgt = cw_rng_range(&rng, 6, 40);
            if (cw_rng_float(&rng) < 0.1f) hgt *= 2;
            const float *c = palette[cw_rng_int(&rng, 0, 5)];
            box(world, x0 + w * (i + 0.5f), hgt / 2, z0 + d * (j + 0.5f), w - 1.2f, hgt, d - 1.2f, c);
          }
      }
      /* lamps at the block's street corners */
      static const float lamp_c[3] = { 0.2f, 0.2f, 0.22f }, bulb_c[3] = { 1, 0.95f, 0.8f };
      for (int k = 0; k < 4; k++) {
        float lx = x0 - STREET / 2 + 1 + (k & 1) * (BLOCK - 2), lz = z0 - STREET / 2 + 1 + (k >> 1) * (BLOCK - 2);
        cw_node pole = prim(world, "CYLINDER", lx, 2.5f, lz, 0.18f, 5, lamp_c);
        cw_node bulb = prim(world, "SPHERE", lx, 5.1f, lz, 0.5f, 0.5f, bulb_c);
        cw_node_set(bulb, "emissive", cw_val_color(1, 0.78741f, 0.31855f, 1));
        if (!no_ranges) cw_node_set(pole, "visibility_range", cw_val_float(150));
        if (!no_ranges) cw_node_set(bulb, "visibility_range", cw_val_float(150));
      }
      /* trees along the block's west street */
      for (int k = 0; k < 3; k++) {
        cw_node t = cw_node_new("MeshInstance3D", "Tree");
        cw_node_set(t, "mesh", (cw_value){ .type = CW_T_RES, .res = tree_uid });
        cw_node3d_set_position(t, (t3_vec3){ x0 - STREET / 2 + 2.2f, 0, z0 + 3 + k * 7.0f });
        cw_node3d_set_rotation(t, (t3_vec3){ 0, cw_rng_range(&rng, 0, CW_TAU), 0 });
        cw_node_set(t, "static", cw_val_bool(true));
        cw_node_set(t, "impostor", cw_val_bool(true));
        cw_node_set(t, "impostor_distance", cw_val_float(80));
        cw_node_set(t, "impostor_fade", cw_val_float(10));
        cw_node_add_child(world, t);
      }
    }
}

typedef struct key { float t, x, y, z, yaw, pitch; } key;
/* the walk: down the street at x = 0 (west of the house block at bx 8,
 * bz 9), into the house's west door, through the inner doorway, back out */
static const key path[] = {
  { 0, 0, 1.7f, 200, 0, 0 },
  { 240, 0, 1.7f, 60, 0.1f, 0 },
  { 300, 0, 1.7f, 45, -CW_PI / 2, 0 },        /* at the door line, facing +x (the house) */
  { 360, 7, 1.7f, 45, -CW_PI / 2, 0 },        /* through the west door into room A */
  { 420, 12, 1.7f, 49, -CW_PI / 2 + 0.3f, -0.05f },
  { 480, 18, 1.7f, 49, -CW_PI / 2, 0 },       /* through the inner doorway into room B */
  { 540, 18, 1.7f, 49, CW_PI / 2, 0 },        /* turn around */
  { 570, 13, 1.7f, 49, CW_PI / 2, 0 },        /* back through the doorway, on its axis */
  { 600, 7, 1.7f, 45, CW_PI / 2, 0 },
  { 660, -3, 1.7f, 45, CW_PI, 0.1f },         /* back on the street, facing +z */
  { 720, -3, 12, 30, CW_PI, -0.3f },          /* and up over the roofs */
};
#define NKEYS ((int)(sizeof path / sizeof path[0]))

static void place_camera(cw_node c, float t) {
  int i = 0;
  while (i < NKEYS - 2 && t > path[i + 1].t) i++;
  const key *a = &path[i], *b = &path[i + 1];
  float u = CW_CLAMP((t - a->t) / (b->t - a->t), 0, 1);
  u = u * u * (3 - 2 * u);
  cw_node3d_set_position(c, (t3_vec3){ cw_lerp(a->x, b->x, u), cw_lerp(a->y, b->y, u), cw_lerp(a->z, b->z, u) });
  cw_node3d_set_rotation(c, (t3_vec3){ cw_lerp(a->pitch, b->pitch, u), cw_lerp(a->yaw, b->yaw, u), 0 });
}

/* the world of one tree: environment, sun, camera, ground, city */
static cw_node build_world(cw_tree *t, cw_node *cam_out, bool ranges) {
  cw_node root = cw_tree_root(t);
  world = cw_node_new("Node3D", "World");
  cw_node_add_child(root, world);
  cw_node env = cw_node_new("WorldEnvironment", "Env");
  cw_node_set(env, "background", cw_val_color(0.34239f, 0.477f, 0.69207f, 1));
  cw_node_set(env, "ambient_energy", cw_val_float(0.94248));
  cw_node_set(env, "hemi_energy", cw_val_float(1.41372));
  cw_node_set(env, "fog_mode", cw_val_enum("LINEAR"));
  cw_node_set(env, "fog_color", cw_val_color(0.34239f, 0.477f, 0.69207f, 1));
  cw_node_set(env, "fog_near", cw_val_float(120));
  cw_node_set(env, "fog_far", cw_val_float(420));
  cw_node_add_child(world, env);
  cw_node sun = cw_node_new("DirectionalLight3D", "Sun");
  cw_node3d_set_rotation(sun, (t3_vec3){ -0.9f, 0.7f, 0 });
  cw_node_set(sun, "energy", cw_val_float(4.39823));
  cw_node_add_child(world, sun);
  cw_node c = cw_node_new("Camera3D", "Camera");
  cw_node_set(c, "fov", cw_val_float(65));
  cw_node_set(c, "near", cw_val_float(0.1));
  cw_node_set(c, "far", cw_val_float(450));
  cw_node_add_child(world, c);
  *cam_out = c;
  cw_node ground = cw_node_new("MeshInstance3D", "Ground");
  cw_node_set(ground, "primitive", cw_val_enum("PLANE"));
  cw_node_set(ground, "size", cw_val_vec3(700, 1, 700));
  cw_node_set(ground, "color", cw_val_color(0.07324f, 0.07829f, 0.07324f, 1));
  cw_node_set(ground, "occlusion_cull", cw_val_bool(false));
  cw_node_add_child(world, ground);
  no_ranges = !ranges;
  build_city();
  return world;
}

static void init(void *u) {
  (void)u;
  tree = cw_scene_app_tree();
  cw_debug_u32("city.tick", &tick);
  cw_debug_i32("city.meshes", &meshes);
  cw_debug_i32("city.visible", &visible);
  cw_debug_i32("city.frustum", &frustum);
  cw_debug_i32("city.occluded", &occluded);
  cw_debug_i32("city.portal", &portal);
  cw_debug_i32("city.distance", &distance);
  cw_debug_i32("city.queries", &queries);
  cw_debug_field("city.lod", lod, CW_DBG_I32, 4);
  cw_debug_i32("city.impostors", &impostors);
  cw_debug_i32("city.rooms_visible", &rooms_vis);
  cw_debug_i32("city.draws", &draws);
  cw_debug_i32("city.bvh_nodes", &bvh_nodes);
  cw_debug_i32("city.static", &static_meshes);
  cw_debug_i32("city.max_pop", &max_pop);
  cw_debug_i32("city.pop_free", &pop_free);
  cw_debug_i32("city.max_pop_tick", &max_pop_tick);
  cw_debug_i32("city.pop_frames", &pop_frames);
  cw_debug_i32("city.queries_late", &late_total);
  cw_debug_field("city.pop_log", pop_log, CW_DBG_I32, 16);
  cw_debug_f32("city.frame_ms", &frame_ms);
  cw_debug_f32("city.cpu_ms", &cpu_ms);
  cw_debug_f32("city.cull_ms", &cull_ms);
  cw_debug_f32("city.draws_avg", &draws_avg);
  cw_debug_f32("city.visible_avg", &visible_avg);
  cw_debug_f32("city.tris_k", &tris_k);
  cw_debug_f32("city.gpu_ms", &gpu_ms);
  const char *m = getenv("VIS_MODE");
  m = m ? m : VIS_DEFAULT_MODE;
  bool off = !strcmp(m, "off");
  check = !strcmp(m, "check") || !strcmp(m, "naive");
  bool cull = !strcmp(m, "cull") || check;
  build_world(tree, &cam, !(off || cull));
  if (!off) {
    mgr = cw_node_new("VisibilityManager", "Vis");
    cw_node_set(mgr, "occlusion_cull", cw_val_bool(!getenv("VIS_NO_OCC")));
    if (getenv("VIS_NO_PORTALS")) cw_node_set(mgr, "portals", cw_val_bool(false));
    if (getenv("VIS_NO_FRUSTUM")) cw_node_set(mgr, "frustum_cull", cw_val_bool(false));
    cw_node_set(mgr, "occlusion_wait", cw_val_bool(true));
    cw_node_set(mgr, "lod", cw_val_bool(!cull));
    cw_node_set(mgr, "impostors", cw_val_bool(!cull));
    if (!strcmp(m, "naive")) cw_node_set(mgr, "occlusion_guard", cw_val_bool(false));
    cw_node_add_child(world, mgr);
  }
  if (check) {
    /* the reference: the same city in a second tree with no culling at all */
    ref_tree = cw_tree_new();
    build_world(ref_tree, &ref_cam, false);
    world = cw_node_get(cw_tree_root(tree), "World");
    int W, H;
    cw_drawable_size(&W, &H);
    ref_rt = cw_rt_new(W, H, CW_RT_RGBA8, true);
    cw_pipeline_set_output(cw_pipeline_of(ref_tree), ref_rt);
    img_a = cw_alloc((size_t)W * H * 4, CW_MEM_GAME);
    img_b = cw_alloc((size_t)W * H * 4, CW_MEM_GAME);
  }
}


static void fixed(void *u, double dt) {
  (void)u;
  (void)dt;
  tick++;
  place_camera(cam, (float)tick);
  if (ref_tree) {
    cw_tree_fixed_update(ref_tree, dt);
    place_camera(ref_cam, (float)tick);
  }
}

static void update(void *u, double dt) {
  (void)u;
  (void)dt;
  t_update = now_ms();
}

static void render(void *u, double a) {
  (void)u;
  (void)a;
  uint8_t px[4];
  glReadPixels(0, 0, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, px); /* wait for the GPU */
  double t = now_ms();
  cw_visibility_stats st;
  cw_visibility_get_stats(tree, &st);
  meshes = st.meshes;
  visible = st.visible;
  frustum = st.frustum_culled;
  occluded = st.occlusion_culled;
  portal = st.portal_culled;
  distance = st.distance_culled;
  queries = st.queries;
  bvh_nodes = st.bvh_nodes;
  static_meshes = st.static_meshes;
  late_total += st.queries_late;
  for (int k = 0; k < 4; k++) lod[k] = st.lod_counts[k];
  impostors = st.impostors;
  rooms_vis = st.rooms_visible;
  cull_ms = st.cull_ms;
  cw_render3d_stats rs;
  cw_render3d_get_stats(tree, &rs);
  draws = rs.draw_calls;
  tris_k = (float)rs.triangles / 1000.0f;
  gpu_ms = cw_pipeline_gpu_ms(cw_pipeline_of(tree));
  if (check) {
    /* the frame A just drew against the reference tree drawn the same way
     * without any culling */
    int W, H;
    cw_drawable_size(&W, &H);
    glPixelStorei(GL_PACK_ALIGNMENT, 1);
    glReadPixels(0, 0, W, H, GL_RGBA, GL_UNSIGNED_BYTE, img_a);
    cw_tree_update(ref_tree, 1.0 / 60);
    cw_tree_pre_render(ref_tree, a);
    cw_pipeline_render(ref_tree, W, H, a);
    glBindFramebuffer(GL_FRAMEBUFFER, ref_rt->fbo);
    glReadPixels(0, 0, W, H, GL_RGBA, GL_UNSIGNED_BYTE, img_b);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    int n = 0;
    for (int i = 0; i < W * H; i++) {
      const uint8_t *p = img_a + 4 * i, *q = img_b + 4 * i;
      int d = abs(p[0] - q[0]);
      d = CW_MAX(d, abs(p[1] - q[1]));
      d = CW_MAX(d, abs(p[2] - q[2]));
      n += d > 24;
    }
    if (n > 4 && pop_logged < 16) pop_log[pop_logged++] = (int32_t)tick * 100000 + CW_MIN(n, 99999);
    if (n > max_pop) {
      max_pop = n;
      max_pop_tick = (int32_t)tick;
    }
    if (n > 64) pop_frames++;
    pop_free = pop_frames <= 1 && max_pop < 1000;
#ifndef __wasm__
    if (getenv("VIS_DUMP") && tick == (uint32_t)atoi(getenv("VIS_DUMP"))) {
      FILE *fa = fopen("out/vis_a.ppm", "wb"), *fb = fopen("out/vis_b.ppm", "wb");
      fprintf(fa, "P6\n%d %d\n255\n", W, H);
      fprintf(fb, "P6\n%d %d\n255\n", W, H);
      for (int y = H - 1; y >= 0; y--)
        for (int x = 0; x < W; x++) {
          fwrite(img_a + 4 * (y * W + x), 1, 3, fa);
          fwrite(img_b + 4 * (y * W + x), 1, 3, fb);
        }
      fclose(fa);
      fclose(fb);
    }
#endif
    if (getenv("VIS_LOG") && n > 4) cw_info("tick %u: %d pixels differ from the unculled city", tick, n);
  }
  if (tick > WARMUP && t_last > 0) {
    timed++;
    sum_frame += t - t_last;
    sum_cpu += t - t_update;
    sum_draws += draws;
    sum_visible += visible;
    draws_avg = (float)(sum_draws / timed);
    visible_avg = (float)(sum_visible / timed);
    frame_ms = (float)(sum_frame / timed);
    cpu_ms = (float)(sum_cpu / timed);
  }
  t_last = t;
}

/* the reference tree is ours: free it before the engine releases the city's
 * glTF (its 768 instances would still be live) */
static void vis_shutdown(void *u) {
  (void)u;
  if (ref_tree) cw_tree_free(ref_tree);
  ref_tree = NULL;
  if (ref_rt) cw_rt_free(ref_rt);
  ref_rt = NULL;
}

cw_app_desc cw_main(int argc, char **argv) {
  (void)argc;
  (void)argv;
  static cw_app_desc base = { .title = "vis_city", .width = 960, .height = 540, .debug = true, .init = init,
                              .fixed_update = fixed, .update = update, .render = render, .shutdown = vis_shutdown };
  return cw_scene_app(NULL, &base);
}

/* lightmap_yard: static lighting baked for a weak GPU, and the cheap
 * stand-ins that keep moving things grounded in it (cartwheel/lightmap.h,
 * cartwheel/splats.h).
 *
 * A yard at dusk: a low sun and two floodlights. The floodlights have
 * bake_direct and realtime off: their light, and the walls' and posts'
 * shadows, live only in the lightmap, and they cost nothing a frame. The sun
 * has bake_direct and stays real time for the ball. LightmapGI's shading
 * BAKED draws the ground unlit from its lightmap (laid on the plane's own
 * uv0: lightmap_uv UV0). The walls and a row of posts are instanced meshes
 * under STATIC MeshInstance3D nodes: they shadow the ground in the bake. A puddle reflects the bake's reflection cube. The
 * rolling ball takes the baked light of the ground under it
 * (cw_lightmap_irradiance_below) as its colour, has a blob shadow, and a
 * lamp over it lights the ground as a light pool.
 *
 * Debug state: yard.tick, yard.hash_lo / yard.hash_hi (the bake's
 * fingerprint, equal on every host), yard.baked (1: the ground draws with
 * the unlit material), yard.cube (the reflection cube's face size),
 * yard.light (the irradiance under the ball, x1000), yard.splats (blob
 * shadows + light pools drawn). */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>

#include "cartwheel/debug.h"
#include "cartwheel/lightmap.h"
#include "cartwheel/math.h"
#include "cartwheel/platform.h"
#include "cartwheel/render3d.h"
#include "cartwheel/scene_file.h"
#include "cartwheel/splats.h"

static int32_t tick, baked, cube, light, nsplats;
static uint32_t hash_lo, hash_hi;
static cw_splats *shadows, *pools;
static bool puddle_made;

/* the first mesh under a three.c object */
static const t3_mesh *first_mesh(const t3_object *o) {
  if (!o) return NULL;
  if (o->type == T3_MESH) return (const t3_mesh *)o;
  for (int i = 0; i < o->child_count; i++) {
    const t3_mesh *m = first_mesh(o->children[i]);
    if (m) return m;
  }
  return NULL;
}

static cw_node node(const char *p) { return cw_node_get(cw_tree_root(cw_scene_app_tree()), p); }

static void init(void *u) {
  (void)u;
  cw_debug_i32("yard.tick", &tick);
  cw_debug_u32("yard.hash_lo", &hash_lo);
  cw_debug_u32("yard.hash_hi", &hash_hi);
  cw_debug_i32("yard.baked", &baked);
  cw_debug_i32("yard.cube", &cube);
  cw_debug_i32("yard.light", &light);
  cw_debug_i32("yard.splats", &nsplats);
  cw_node yard = node("Yard");
  /* YARD_LIT=1 (the gate's control): the lightmapped meshes keep their lit material, so the bake-only floodlights' pools are gone */
  if (getenv("YARD_LIT")) cw_node_set(node("Yard/GI"), "shading", cw_val_enum("LIT"));
  /* aim the camera and the lights (they shine down their -Z) */
  cw_node3d_look_at(node("Yard/Camera"), (t3_vec3){ 0, 0, 0.5f }, (t3_vec3){ 0, 1, 0 });
  cw_node3d_look_at(node("Yard/Sun"), (t3_vec3){ 6, -2.2f, -3 }, (t3_vec3){ 0, 1, 0 }); /* low, from the left and behind */
  cw_node3d_look_at(node("Yard/FloodA"), (t3_vec3){ -2.5f, 0, 1 }, (t3_vec3){ 0, 1, 0 });
  cw_node3d_look_at(node("Yard/FloodB"), (t3_vec3){ 3, 0, 0.5f }, (t3_vec3){ 0, 1, 0 });
  { /* the ground: a 20 m plane, facing up */
    t3_geometry *pg = t3_plane_geometry_new(20, 20, 8, 8);
    t3_geometry_apply_matrix4(pg, &(t3_mat4){ { 1, 0, 0, 0, 0, 0, -1, 0, 0, 1, 0, 0, 0, 0, 0, 1 } });
    t3_material *gm = t3_mesh_standard_material_new(0x6b6661);
    gm->roughness = 0.9f;
    t3_mesh *gmesh = t3_mesh_new(pg, gm);
    cw_mesh3d_set_procedural(node("Yard/Ground"), gmesh);
    t3_release(gmesh), t3_release(gm), t3_release(pg);
  }
  { /* two walls: an instanced mesh, STATIC (it shadows the ground in the bake and stays lit in real time) */
    t3_material *wm = t3_mesh_standard_material_new(0x9a8f86);
    wm->roughness = 0.9f;
    t3_instanced_mesh *wi = t3_instanced_mesh_new(t3_box_geometry_new(1, 1, 1, 1, 1, 1), wm, 2);
    t3_mat4 m;
    t3_mat4_compose(&m, (t3_vec3){ 0, 1, -7 }, (t3_quat){ 0, 0, 0, 1 }, (t3_vec3){ 12, 2, 0.5f });
    t3_instanced_mesh_set_matrix_at(wi, 0, &m);
    t3_mat4_compose(&m, (t3_vec3){ -7, 0.75f, 0 }, (t3_quat){ 0, 0, 0, 1 }, (t3_vec3){ 0.5f, 1.5f, 8 });
    t3_instanced_mesh_set_matrix_at(wi, 1, &m);
    t3_release(wm);
    cw_node walls = cw_node_new("MeshInstance3D", "Walls");
    cw_node_add_child(yard, walls);
    cw_node_set(walls, "gi_mode", cw_val_enum("STATIC"));
    cw_mesh3d_set_procedural(walls, &wi->mesh);
    t3_release(wi);
  }
  /* the posts: one instanced mesh, STATIC, so the bake sees every post */
  t3_material *pm = t3_mesh_standard_material_new(0x8a8580);
  pm->roughness = 0.8f;
  t3_instanced_mesh *im = t3_instanced_mesh_new(t3_box_geometry_new(0.3f, 2.4f, 0.3f, 1, 1, 1), pm, 6);
  for (int i = 0; i < 6; i++) {
    t3_mat4 m;
    t3_mat4_compose(&m, (t3_vec3){ -5 + 2 * (float)i, 1.2f, -3 }, (t3_quat){ 0, 0, 0, 1 }, (t3_vec3){ 1, 1, 1 });
    t3_instanced_mesh_set_matrix_at(im, i, &m);
  }
  t3_release(pm);
  cw_node posts = cw_node_new("MeshInstance3D", "Posts");
  cw_node_add_child(yard, posts);
  cw_node_set(posts, "gi_mode", cw_val_enum("STATIC"));
  cw_mesh3d_set_procedural(posts, &im->mesh);
  t3_release(im);
  /* the lamp over the ball: real time, not baked: a light pool on the baked ground */
  cw_node lamp = cw_node_new("OmniLight3D", "Lamp");
  cw_node_add_child(yard, lamp);
  cw_node_set(lamp, "color", cw_val_color(1, 0.6f, 0.3f, 1));
  cw_node_set(lamp, "energy", cw_val_float(6));
  cw_node_set(lamp, "range", cw_val_float(4));
  cw_node_set(lamp, "bake", cw_val_bool(false));
  shadows = cw_splats_new(yard, "Shadows", CW_SPLATS_SHADOW, 8);
  pools = cw_splats_new(yard, "Pools", CW_SPLATS_LIGHT, 8);
}

static t3_vec3 ball_at(int t) { return (t3_vec3){ -4 + 8 * (float)(t % 240) / 240.0f, 0.5f, 1.5f - 3.0f * cw_sin((float)t * 0.026f) }; }

static void fixed_update(void *u, double dt) {
  (void)u;
  (void)dt;
  tick++;
  t3_vec3 b = ball_at(tick);
  cw_node3d_set_position(node("Yard/Ball"), b);
  cw_node3d_set_position(node("Yard/Lamp"), (t3_vec3){ b.x, 1.8f, b.z });
}

static void render(void *u, double alpha) {
  (void)u;
  (void)alpha;
  cw_node gi = node("Yard/GI");
  const cw_lightmap_data *d = cw_lightmap_gi_data(gi);
  cw_value v;
  if (cw_node_get_prop(gi, "hash", &v) && v.s && v.s[0]) {
    unsigned long long h = strtoull(v.s, NULL, 16);
    hash_lo = (uint32_t)h;
    hash_hi = (uint32_t)(h >> 32);
  }
  const t3_mesh *g = first_mesh(cw_node3d_object(node("Yard/Ground")));
  baked = g && g->material_count && g->materials[0]->type == T3_MESH_BASIC_MATERIAL;
  t3_vec3 b = ball_at(tick);
  if (d && !puddle_made) { /* a puddle: dark water reflecting the bake's cube */
    puddle_made = true;
    float s;
    cube = cw_lightmap_reflection(d, &s) ? 16 : 0;
    t3_material *m = t3_mesh_basic_material_new(0x101418);
    m->transparent = true;
    m->opacity = 0.85f;
    m->depth_write = false;
    cw_lightmap_use_reflection(d, m, 1);
    t3_geometry *pg = t3_circle_geometry_new(1.4f, 32);
    t3_geometry_apply_matrix4(pg, &(t3_mat4){ { 1, 0, 0, 0, 0, 0, -1, 0, 0, 1, 0, 0, 0, 0, 0, 1 } }); /* face +Y */
    t3_mesh *pm = t3_mesh_new(pg, m);
    pm->base.position = (t3_vec3){ 2.5f, 0.012f, 2.5f };
    cw_node pn = cw_node_new("Node3D", "Puddle");
    cw_node_add_child(node("Yard"), pn);
    t3_object *w = t3_group_new();
    t3_object_add(w, pm);
    cw_node3d_set_object(pn, w);
    t3_release(w), t3_release(pm), t3_release(pg), t3_release(m);
  }
  t3_color e = { 0, 0, 0 };
  if (d && cw_lightmap_irradiance_below(d, b, &e)) { /* the ball takes the ground's baked light */
    float k = CW_CLAMP((e.r + e.g + e.b) / 3.0f, 0.15f, 3.0f);
    cw_node_set(node("Yard/Ball"), "color", cw_val_color(0.8f * CW_MIN(e.r / (e.g + 1e-4f), 1.5f) * k, 0.8f * k, 0.8f * CW_MIN(e.b / (e.g + 1e-4f), 1.5f) * k, 1));
  }
  light = (int32_t)((e.r + e.g + e.b) * 1000.0f / 3.0f);
  cw_splats_begin(shadows);
  cw_splats_add(shadows, (t3_vec3){ b.x, 0, b.z }, 1.6f, 1.0f, 1.2f, 0.25f, (t3_color){ 1, 1, 1 }, 0.6f);
  cw_splats_end(shadows);
  cw_splats_begin(pools);
  cw_splats_add_lights(pools, node("Yard"), 0, 0.4f / 3.14159265f);
  cw_splats_end(pools);
  nsplats = cw_splats_count(shadows) + cw_splats_count(pools);
}

cw_app_desc cw_main(int argc, char **argv) {
  (void)argc;
  (void)argv;
  static cw_app_desc base = { .title = "lightmap_yard", .width = 640, .height = 360, .debug = true, .init = init,
                              .fixed_update = fixed_update, .render = render };
  return cw_scene_app("main.cwscene", &base);
}

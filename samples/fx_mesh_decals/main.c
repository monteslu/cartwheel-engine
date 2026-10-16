/* fx_mesh_decals: Decal3D in MESH mode. Each decal is clipped from the
 * meshes it lands on when it spawns and lives on as small meshes under them:
 *
 *   Badge, Paint  on a walking skinned character (Khronos CesiumMan,
 *                 CC-BY-4.0, from deps/three.c's test assets): clipped from
 *                 the pose at spawn, drawn skinned on its skeleton, so they
 *                 swing with the body (MIX and MULTIPLY)
 *   Crack         on a transparent glass pane, which the screen-space pass
 *                 never reaches
 *   Ring          a glow (ADD) stamped on a crate with no target (every mesh
 *                 the box overlaps); the crate spins and the ring turns with it
 *   holes         bullet holes spawned on the wall every 6 ticks, pooled with
 *                 max_mesh_decals = 12: the oldest is freed past twelve
 *
 * Debug state: md.decals / tris / pieces / recycled / builds (counts),
 * md.hash (every mesh decal's geometry, FNV over the vertex bits),
 * md.hero_hash / md.hero_tris (the two decals on the character),
 * md.glass_tris, md.ring_tris, md.holes (spawned), md.tick. Controls:
 * FX_MESH_SHIFT=1 moves every decal 0.2 m to the right before it spawns
 * (other geometry: the hashes change), FX_MESH_OFF=1 turns every decal back
 * to screen space (no mesh geometry, and the glass loses its crack).
 * FX_MESH_BENCH=1 clips the character's decals again every frame and reports
 * the mean CPU ms of one clip (md.clip_ms) and the character's triangles
 * (md.hero_src_tris). */
#include <stdlib.h>
#include <string.h>

#include "cartwheel/anim.h"
#include "cartwheel/debug.h"
#include "cartwheel/decals.h"
#include "cartwheel/render3d.h"
#include "cartwheel/scene_file.h"

static t3_gltf *gltf;
static bool glass_done, shift, off;
static uint32_t tick, mesh_hash, hero_hash;
static int32_t n_decals, n_tris, n_pieces, n_recycled, n_builds, hero_tris, glass_tris, ring_tris, holes;
static cw_rng rng;
/* timing (FX_MESH_BENCH=1 clips the character's two decals again every
 * frame): never compared across hosts */
static bool bench;
static float clip_ms_sum, clip_ms;
static int32_t clip_n, hero_src_tris;

static cw_node world(void) { return cw_node_get(cw_tree_root(cw_scene_app_tree()), "World"); }

/* the controls change a decal before it enters the tree */
static void adjust(cw_node d) {
  if (shift) {
    t3_vec3 p = cw_node3d_position(d);
    cw_node3d_set_position(d, (t3_vec3){ p.x + 0.2f, p.y, p.z });
  }
  if (off) cw_node_set(d, "mode", cw_val_enum("SCREEN"));
}

static void init(void *u) {
  (void)u;
  const char *e = getenv("FX_MESH_SHIFT");
  shift = e && *e == '1';
  e = getenv("FX_MESH_OFF");
  off = e && *e == '1';
  e = getenv("FX_MESH_BENCH");
  bench = e && *e == '1';
  cw_rng_seed(&rng, 77);
  cw_node w = world();
  /* the character: the glTF scene as the Model node's object, its walk on loop */
  size_t len;
  void *glb = cw_asset_load("CesiumMan.glb", &len);
  if (!glb) {
    cw_error("fx_mesh_decals: CesiumMan.glb missing from the assets");
    return;
  }
  gltf = t3_gltf_parse(glb, len, NULL, NULL); /* kept: its objects use names the t3_gltf owns */
  free(glb);
  cw_node3d_set_object(cw_node_get(w, "Hero/Model"), gltf->scene);
  cw_anim walk = cw_anim_from_t3_clip(gltf->animations[0], "Hero/Model");
  cw_anim_set_loop(walk, CW_LOOP_LINEAR);
  cw_node anim = cw_node_get(w, "Anim");
  cw_anim_player_add(anim, "walk", walk);
  cw_anim_release(walk);
  cw_anim_player_play(anim, "walk", 0);

  /* the scene's decals spawn at the first render: the controls apply now */
  for (int i = 0; i < cw_node_child_count(w); i++)
    if (cw_node_is(cw_node_child(w, i), "Decal3D")) adjust(cw_node_child(w, i));

  cw_debug_u32("md.tick", &tick);
  cw_debug_i32("md.decals", &n_decals);
  cw_debug_i32("md.tris", &n_tris);
  cw_debug_i32("md.pieces", &n_pieces);
  cw_debug_i32("md.recycled", &n_recycled);
  cw_debug_i32("md.builds", &n_builds);
  cw_debug_u32("md.hash", &mesh_hash);
  cw_debug_u32("md.hero_hash", &hero_hash);
  cw_debug_i32("md.hero_tris", &hero_tris);
  cw_debug_i32("md.glass_tris", &glass_tris);
  cw_debug_i32("md.ring_tris", &ring_tris);
  cw_debug_i32("md.holes", &holes);
  cw_debug_f32("md.clip_ms", &clip_ms);
  cw_debug_i32("md.clip_n", &clip_n);
  cw_debug_i32("md.hero_src_tris", &hero_src_tris);
}

/* the pane's material, made see-through (MeshInstance3D draws opaque
 * materials; a glTF with a BLEND material is the usual source of glass) */
static void make_glass(void) {
  t3_object *g = cw_node3d_object(cw_node_get(world(), "Glass"));
  if (!g || !g->child_count || g->children[0]->type != T3_MESH) return;
  t3_mesh *m = (t3_mesh *)g->children[0];
  t3_material *glass = t3_material_clone(m->materials[0]);
  glass->transparent = true;
  glass->opacity = 0.3f;
  glass->depth_write = false;
  t3_release(m->materials[0]);
  m->materials[0] = glass;
  glass_done = true;
}

static void fixed(void *u, double dt) {
  (void)u;
  (void)dt;
  tick++;
  if (!glass_done) make_glass();
  cw_node crate = cw_node_get(world(), "Crate");
  cw_node3d_set_rotation(crate, (t3_vec3){ 0, (float)tick * 0.01f, 0 });
  /* #region holes */
  if (tick % 6 == 0 && tick <= 150) {
    cw_node d = cw_node_new("Decal3D", "Hole");
    float x = cw_rng_range(&rng, -1.6f, 3.4f);
    float y = cw_rng_range(&rng, 0.3f, 2.7f);
    float s = cw_rng_range(&rng, 0.12f, 0.22f);
    cw_node3d_set_position(d, (t3_vec3){ x, y, -1.5f });
    cw_node_set(d, "size", cw_val_vec3(s, s, 0.3f));
    cw_node_set(d, "color", cw_val_color(0.12f, 0.1f, 0.09f, 1));
    cw_node_set(d, "blend", cw_val_enum("MULTIPLY"));
    cw_node_set(d, "mode", cw_val_enum("MESH"));
    cw_node_set(d, "max_mesh_decals", cw_val_int(12));
    adjust(d);
    cw_node_add_child(world(), d);
    holes++;
  }
  /* #endregion */
}

static int32_t tris_of(const char *name, uint32_t *hash) {
  cw_decal_mesh_info info;
  if (!cw_decal_get_mesh_info(cw_node_get(world(), name), &info)) return 0;
  if (bench && info.built) {
    clip_ms_sum += info.build_ms;
    clip_n++;
    clip_ms = clip_ms_sum / (float)clip_n;
    cw_decal_restamp(cw_node_get(world(), name));
  }
  if (hash) *hash = (*hash ^ info.hash) * 16777619u;
  return info.triangles;
}

static void render(void *u, double a) {
  (void)u;
  (void)a;
  cw_decal_stats st;
  cw_decals_get_stats(cw_scene_app_tree(), &st);
  n_decals = st.mesh_decals;
  n_tris = st.mesh_triangles;
  n_pieces = st.mesh_pieces;
  n_recycled = st.mesh_recycled;
  n_builds = st.mesh_builds;
  mesh_hash = st.mesh_hash;
  hero_hash = 2166136261u;
  hero_tris = tris_of("Badge", &hero_hash) + tris_of("Paint", &hero_hash);
  glass_tris = tris_of("Crack", NULL);
  if (gltf && !hero_src_tris) {
    /* the character's triangles: what each of its decals clips */
    t3_object *stack[64];
    int sp = 0;
    stack[sp++] = gltf->scene;
    while (sp) {
      t3_object *o = stack[--sp];
      bool piece = o->name && !strcmp(o->name, "Decal3D mesh");
      if (o->type == T3_SKINNED_MESH && !piece && ((t3_mesh *)o)->geometry->index)
        hero_src_tris += ((t3_mesh *)o)->geometry->index->count / 3;
      for (int i = 0; i < o->child_count && sp < 64; i++) stack[sp++] = o->children[i];
    }
  }
  ring_tris = tris_of("Ring", NULL);
}

static void shutdown(void *u) {
  (void)u;
  if (gltf) t3_release(gltf);
  gltf = NULL;
}

cw_app_desc cw_main(int argc, char **argv) {
  (void)argc;
  (void)argv;
  static cw_app_desc base = { .title = "fx_mesh_decals", .width = 640, .height = 360, .debug = true, .init = init,
                              .fixed_update = fixed, .render = render, .shutdown = shutdown };
  return cw_scene_app("main.cwscene", &base);
}

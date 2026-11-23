/* runtime_nodes: nodes that own render passes or per-frame GPU work, added from code while the game runs, draw as the
 * same nodes declared in a scene file do, in the main tree, in a Backdrop's world and in a SubViewport's world.
 *
 * The world (world.cwscene: a yard with a sun, a camera, a floor and a crate) goes in on the first tick the test
 * switches are seen. Its content (content.cwscene, under one Node3D "Group") is a node of every such type: GPU
 * particles with an attractor, a trail, a screen and a mesh decal, an omni, a spot and a cascaded sun, height fog
 * with a fog volume, a shockwave, an explosion, water, a reflection probe, a light probe grid and cloth.
 *
 *   RT_TREE main         the world instanced into this tree
 *   RT_TREE backdrop     the world in a Backdrop node's world (cartwheel/backdrop.h), drawn behind this tree
 *   RT_TREE subviewport  the world in a SubViewport's world (cartwheel/subviewport.h), shown full screen
 *   RT_ADD scene         the world is world_decl.cwscene, the content declared in it (hidden: a hidden node's clocks
 *                        wait), and shown RT_AT ticks later (default 30): the reference
 *   RT_ADD code          (or unset) the world is world.cwscene; RT_AT ticks later every node of content.cwscene is
 *                        made again with cw_node_new, its properties set one by one (those a scene file sets) and added
 *                        to the world: the picture must be the reference's
 *   RT_ADD none          nothing added (the control: the content must show)
 *
 *   RT_MISSING 1        a toon-shaded box (a program state no table here has) goes into the world with the content and
 *                        another into this tree 30 ticks later: both trees' skipped states must be reported together
 *                        (cw.three.missing_states, cw_pipeline_missing_states), whichever tree changed last
 *
 * (cw_test_env reads nothing in a cart's first frame: that frame draws the empty main tree in every mode.)
 *
 * Debug state: rt.tree, rt.add (the modes, 0 1 2), rt.added_tick (the tick the content went in or was shown), rt.tick,
 * and per type, summed over the frames since: rt.p3_draws, rt.dec_drawn, rt.fog_volumes, rt.water_surfaces,
 * rt.gi_captures, rt.boom_flashes, rt.cloth_hash (now) read from the tree the world is in. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cartwheel/backdrop.h"
#include "cartwheel/debug.h"
#include "cartwheel/decals.h"
#include "cartwheel/environment.h"
#include "cartwheel/explosions.h"
#include "cartwheel/gi.h"
#include "cartwheel/math.h"
#include "cartwheel/particles.h"
#include "cartwheel/render3d.h"
#include "cartwheel/scene_file.h"
#include "cartwheel/subviewport.h"
#include "cartwheel/water.h"

enum { T_MAIN, T_BACKDROP, T_SUBVIEWPORT };
enum { A_SCENE, A_CODE, A_NONE };
static int32_t tree_mode = -1, add_mode, added_tick, p3_draws, dec_drawn, fog_volumes, water_surfaces, gi_captures, boom_flashes,
    cloth_hash;
static uint32_t tick, at_tick;
static bool missing_mode;
static cw_node holder, world;
static cw_tree *wtree;

static cw_node main_root(void) { return cw_node_get(cw_tree_root(cw_scene_app_tree()), "Main"); }

static void init(void *u) {
  (void)u;
#define F(name) cw_debug_i32("rt." #name, &name)
  F(add_mode), F(added_tick), F(p3_draws), F(dec_drawn), F(fog_volumes), F(water_surfaces), F(gi_captures), F(boom_flashes), F(cloth_hash);
#undef F
  cw_debug_i32("rt.tree", &tree_mode);
  cw_debug_u32("rt.tick", &tick);
}

/* #region clone */
/* a node made again from code: the same type, and every property that differs from a new node's set by name, as game
 * code building it would (and as a scene file does); its children the same way */
static cw_node clone(cw_node src) {
  const cw_node_type *t = cw_node_get_type(src);
  cw_node n = cw_node_new(t->name, cw_node_name(src));
  for (const cw_node_type *c = t; c; c = c->base)
    for (const cw_prop *p = c->props; p && p->name; p++) {
      if ((p->flags & CW_PROP_READONLY) || (p->offset < 0 && !p->set)) continue;
      cw_value a, b;
      if (!cw_node_get_prop(src, p->name, &a) || !cw_node_get_prop(n, p->name, &b)) continue;
      char x[512], y[512];
      cw_value_format(&a, x, sizeof x);
      cw_value_format(&b, y, sizeof y);
      if (strcmp(x, y)) cw_node_set(n, p->name, a);
    }
  for (int i = 0; i < cw_node_child_count(src); i++) cw_node_add_child(n, clone(cw_node_child(src, i)));
  return n;
}
/* #endregion */

/* the world into the tree RT_TREE names */
static void setup(void) {
  const char *tr = cw_test_env("RT_TREE"), *ad = cw_test_env("RT_ADD"), *at = cw_test_env("RT_AT");
  tree_mode = tr && !strcmp(tr, "backdrop") ? T_BACKDROP : tr && !strcmp(tr, "subviewport") ? T_SUBVIEWPORT : T_MAIN;
  add_mode = ad && !strcmp(ad, "scene") ? A_SCENE : ad && !strcmp(ad, "none") ? A_NONE : A_CODE;
  at_tick = tick + (uint32_t)(at ? atoi(at) : 30);
  const char *scene = add_mode == A_SCENE ? "world_decl.cwscene" : "world.cwscene";
  cw_node m = main_root();
  if (tree_mode == T_MAIN) {
    cw_node_free(cw_node_get(m, "Camera")); /* (the world's camera is the view) */
    world = cw_scene_instance_path(scene);
    cw_node_add_child(m, world);
    wtree = cw_scene_app_tree();
  } else if (tree_mode == T_BACKDROP) {
    holder = cw_node_new("Backdrop", "Far");
    cw_node_set(holder, "scene", cw_val_string(scene));
    cw_node_add_child(m, holder);
    world = cw_backdrop_root(holder);
    wtree = cw_backdrop_tree(holder);
  } else {
    holder = cw_node_new("SubViewport", "View");
    cw_node_set(holder, "scene", cw_val_string(scene));
    cw_node_set(holder, "size", cw_val_vec2(640, 360));
    cw_node_add_child(m, holder);
    cw_node rect = cw_node_new("TextureRect", "Shown");
    cw_node_set(rect, "anchor_right", cw_val_float(1));
    cw_node_set(rect, "anchor_bottom", cw_val_float(1));
    cw_node_set(rect, "expand", cw_val_bool(true));
    cw_node_set(rect, "texture", cw_val_string("viewport://View"));
    cw_node_add_child(cw_node_get(m, "HUD"), rect);
    world = cw_subviewport_root(holder);
    wtree = cw_subviewport_tree(holder);
  }
}

/* RT_AT ticks on: the content shown (declared) or added from code */
static void add_content(void) {
  added_tick = (int32_t)tick;
  if (add_mode == A_SCENE) {
    cw_node_set(cw_node_get(world, "Group"), "visible", cw_val_bool(true));
  } else if (add_mode == A_CODE) {
    /* #region add */
    cw_node tmpl = cw_scene_instance_path("content.cwscene"); /* (in no tree: what the code copies) */
    cw_node_add_child(world, clone(tmpl));
    cw_node_free(tmpl);
    /* #endregion */
  }
}

/* RT_MISSING: a box drawn with a state no program table here has (toon shading) */
static void add_unsurveyed(cw_node parent, const char *name, float x) {
  cw_node b = cw_node_new("MeshInstance3D", name);
  cw_node_set(b, "shading", cw_val_enum("TOON"));
  cw_node_add_child(parent, b);
  cw_node3d_set_position(b, (t3_vec3){ x, 0.5f, 3 });
}

static void fixed(void *u, double dt) {
  (void)u;
  (void)dt;
  tick++;
  if (tree_mode < 0 && cw_test_env("RT_TREE")) setup();
  if (tree_mode < 0 && tick >= 3 && !cw_test_env("RT_TREE")) setup(); /* (no switches: the program survey's run) */
  if (tree_mode < 0) return;
  if (!added_tick && tick >= at_tick) {
    add_content();
    const char *mm = cw_test_env("RT_MISSING");
    missing_mode = mm && atoi(mm);
    if (missing_mode) add_unsurveyed(world, "ToonW", 2);
  }
  if (missing_mode && tick == at_tick + 30 && tree_mode != T_MAIN) add_unsurveyed(main_root(), "ToonM", -2);
  /* the trail's node goes round, so a ribbon shows where it was */
  cw_node mover = cw_node_get(world, "Group/Mover");
  if (cw_node_valid(mover)) {
    float s, c;
    cw_sincos((float)tick * 0.06f, &s, &c);
    cw_node3d_set_position(mover, (t3_vec3){ 2 + 1.5f * c, 1.5f, 1 + 1.5f * s });
  }
}

static void render(void *u, double alpha) {
  (void)u;
  (void)alpha;
  if (!wtree || !added_tick) return;
  cw_particles_stats ps;
  cw_particles_get_stats(wtree, &ps);
  p3_draws += ps.draw_calls;
  cw_decal_stats ds;
  cw_decals_get_stats(wtree, &ds);
  dec_drawn += ds.visible;
  cw_volumetric_fog_state fs;
  cw_volumetric_fog_get_state(wtree, &fs);
  fog_volumes += fs.volumes;
  cw_water_stats ws;
  cw_water_get_stats(wtree, &ws);
  water_surfaces += ws.surfaces;
  cw_gi_stats gs;
  cw_gi_get_stats(wtree, &gs);
  gi_captures = gs.captures_total;
  cw_explosion_stats es;
  cw_explosion_get_stats(wtree, &es);
  boom_flashes += es.lights_lit > 0;
  cw_value v;
  if (cw_node_get_prop(cw_node_get(world, "Group/Flag"), "hash", &v)) cloth_hash = (int32_t)v.i;
}

cw_app_desc cw_main(int argc, char **argv) {
  (void)argc;
  (void)argv;
  static cw_app_desc base = { .title = "runtime_nodes", .width = 640, .height = 360, .debug = true,
                              .init = init, .fixed_update = fixed, .render = render };
  return cw_scene_app("main.cwscene", &base);
}

/* hidden_vis: the visibility rule (cw_node_visible_in_tree, cartwheel/scene.h) for every node type that draws or
 * runs per frame outside the mesh path. Every such node sits under one Node3D, Group (a CanvasLayer's Node2D,
 * Group2D, for the 2D particles), and the scene starts with both hidden.
 *
 *   HV_MODE hidden            both stay hidden: the picture must be the one `absent` draws, and none of the
 *                             counters below may move
 *   HV_MODE absent            both groups freed on the first tick the mode is seen (the reference picture)
 *   HV_MODE show or unset     hidden until tick HV_SHOW (default 90), then shown: everything draws and runs again
 *   HV_MODE visible           shown on the first tick the mode is seen
 *   HV_MODE self              the groups shown and every node in them hidden by its own `visible` instead (its parents
 *                             shown; SubViewport and Backdrop, plain Nodes with no `visible`, are freed): the same as hidden
 *
 * (cw_test_env reads nothing in the first frame, so every mode draws that frame with the groups hidden.)
 *
 * Debug state, summed over the run unless named "now":
 *   hv.p3_steps / hv.p3_draws   GPU particle simulation steps and particle + trail draws
 *   hv.p3_hidden                particle systems hidden now
 *   hv.flames_alive             the Flames system's alive count now (frozen while hidden)
 *   hv.dec_drawn / hv.dec_draws screen-space decals drawn, their draw calls
 *   hv.dec_hidden               decals hidden now; hv.dec_builds mesh decal clips so far; hv.dec_tris mesh decal triangles now
 *   hv.cascades                 frames the hidden cascaded sun had cascades
 *   hv.fog_volumes / fog_lights / fog_shadowed   volumetric fog volumes, lights and shadowed lights, summed per frame
 *                               (lights saturates at the fog's cap: the explosion's pooled flash lights fill it)
 *   hv.shock_age_ms             the shockwave's age now (ms)
 *   hv.boom_flashes             frames an explosion flash light was lit
 *   hv.water_surfaces / hv.water_hidden   Water3D surfaces drawn (summed) / hidden now
 *   hv.gi_captures / hv.gi_grid reflection probe captures, probe grid probe updates; hv.gi_hidden probes hidden now
 *   hv.cloth_hash               the flag's simulation hash now (0 until it first steps)
 *   hv.p2_alive                 the 2D particles alive now
 *   hv.sv_renders               the SubViewport's renders
 *   hv.bd_renders               the Backdrop's renders (a world drawn behind the scene)
 *   hv.on_screen                frames the notifier reported on screen
 *   hv.draws                    three.c draw calls of the last frame
 *   hv.shown_tick               the tick the groups were shown (0: never)
 *   hv.tick                     fixed ticks */
#include <stdlib.h>
#include <string.h>

#include "cartwheel/backdrop.h"
#include "cartwheel/debug.h"
#include "cartwheel/decals.h"
#include "cartwheel/environment.h"
#include "cartwheel/explosions.h"
#include "cartwheel/gameplay.h"
#include "cartwheel/gi.h"
#include "cartwheel/lighting.h"
#include "cartwheel/math.h"
#include "cartwheel/particles.h"
#include "cartwheel/platform.h"
#include "cartwheel/render3d.h"
#include "cartwheel/scene_file.h"
#include "cartwheel/subviewport.h"
#include "cartwheel/water.h"

static int32_t p3_steps, p3_draws, p3_hidden, flames_alive, dec_drawn, dec_draws, dec_hidden, dec_builds, dec_tris,
    cascades, fog_volumes, fog_lights, fog_shadowed, shock_age_ms, boom_flashes, water_surfaces, water_hidden, gi_captures, gi_grid,
    gi_hidden, cloth_hash, p2_alive, sv_renders, bd_renders, on_screen, draws, shown_tick;
static uint32_t tick;
static bool decided, freed;

static cw_node node(const char *path) { return cw_node_get(cw_tree_root(cw_scene_app_tree()), path); }

static void init(void *u) {
  (void)u;
#define F(name) cw_debug_i32("hv." #name, &name)
  F(p3_steps), F(p3_draws), F(p3_hidden), F(flames_alive), F(dec_drawn), F(dec_draws), F(dec_hidden), F(dec_builds),
      F(dec_tris), F(cascades), F(fog_volumes), F(fog_lights), F(fog_shadowed), F(shock_age_ms), F(boom_flashes), F(water_surfaces),
      F(water_hidden), F(gi_captures), F(gi_grid), F(gi_hidden), F(cloth_hash), F(p2_alive), F(sv_renders), F(bd_renders), F(on_screen),
      F(draws), F(shown_tick);
#undef F
  cw_debug_u32("hv.tick", &tick);
}

static void set_shown(bool on) {
  if (on) shown_tick = (int32_t)tick;
  cw_node_set(node("World/Group"), "visible", cw_val_bool(on));
  cw_node_set(node("World/HUD/Group2D"), "visible", cw_val_bool(on));
}

/* every node under n with no children of its own hides itself (its parents stay shown) */
static void hide_leaves(cw_node n) {
  for (int i = 0; i < cw_node_child_count(n); i++) {
    cw_node c = cw_node_child(n, i);
    if (cw_node_child_count(c)) hide_leaves(c);
    else cw_node_set(c, "visible", cw_val_bool(false)); /* (SubViewport and Backdrop have no `visible`: they stay) */
  }
}

static void fixed(void *u, double dt) {
  (void)u;
  (void)dt;
  tick++;
  const char *mode = cw_test_env("HV_MODE");
  if (mode && !decided) {
    decided = true;
    if (!strcmp(mode, "absent")) {
      cw_node_free(node("World/Group"));
      cw_node_free(node("World/HUD/Group2D"));
      freed = true;
    } else if (!strcmp(mode, "visible")) {
      set_shown(true);
    } else if (!strcmp(mode, "self")) {
      hide_leaves(node("World/Group"));
      hide_leaves(node("World/HUD/Group2D"));
      cw_node_free(node("World/Group/View")); /* (plain Nodes: no `visible` of their own to hide them by) */
      cw_node_free(node("World/Group/Far"));
      set_shown(true);
    }
  }
  if (!mode || !strcmp(mode, "show")) { /* (unset: show, so a plain run, the program survey's, draws everything) */
    const char *s = cw_test_env("HV_SHOW");
    if (tick == (uint32_t)(s ? atoi(s) : 90)) set_shown(true);
  }
  /* the trail's node goes round, so a ribbon shows where it was */
  if (!freed) {
    float s, c;
    cw_sincos((float)tick * 0.06f, &s, &c);
    cw_node3d_set_position(node("World/Group/Mover"), (t3_vec3){ 2 + 1.5f * c, 1.5f, 1 + 1.5f * s });
  }
}

static void render(void *u, double alpha) {
  (void)u;
  (void)alpha;
  cw_tree *t = cw_scene_app_tree();
  cw_particles_stats ps;
  cw_particles_get_stats(t, &ps);
  p3_steps += ps.sim_steps;
  p3_draws += ps.draw_calls;
  p3_hidden = ps.hidden;
  cw_decal_stats ds;
  cw_decals_get_stats(t, &ds);
  dec_drawn += ds.visible;
  dec_draws += ds.draw_calls;
  dec_hidden = ds.hidden;
  dec_builds = ds.mesh_builds;
  dec_tris = ds.mesh_triangles;
  cw_lighting_stats ls;
  cw_lighting_get_stats(t, &ls);
  cascades += ls.cascades > 0;
  cw_volumetric_fog_state fs;
  cw_volumetric_fog_get_state(t, &fs);
  fog_volumes += fs.volumes;
  fog_lights += fs.lights;
  fog_shadowed += fs.shadowed_lights;
  cw_explosion_stats es;
  cw_explosion_get_stats(t, &es);
  boom_flashes += es.lights_lit > 0;
  cw_water_stats ws;
  cw_water_get_stats(t, &ws);
  water_surfaces += ws.surfaces;
  water_hidden = ws.hidden;
  cw_gi_stats gs;
  cw_gi_get_stats(t, &gs);
  gi_captures = gs.captures_total;
  gi_grid += gs.grid_updates;
  gi_hidden = gs.hidden;
  cw_render3d_stats rs;
  cw_render3d_get_stats(t, &rs);
  draws = rs.draw_calls;
  if (!freed) {
    cw_value v;
    flames_alive = cw_particles_alive(node("World/Group/Flames"));
    if (cw_node_get_prop(node("World/Group/Wave"), "age", &v)) shock_age_ms = (int32_t)(v.f * 1000 + 0.5);
    if (cw_node_get_prop(node("World/Group/Flag"), "hash", &v)) cloth_hash = (int32_t)v.i;
    p2_alive = cw_particles_alive(node("World/HUD/Group2D/Sparks"));
    sv_renders = cw_subviewport_renders(node("World/Group/View"));
    bd_renders = cw_backdrop_renders(node("World/Group/Far"));
    on_screen += cw_notifier_is_on_screen(node("World/Group/Seen"));
  }
}

cw_app_desc cw_main(int argc, char **argv) {
  (void)argc;
  (void)argv;
  static cw_app_desc base = { .title = "hidden_vis", .width = 640, .height = 360, .debug = true,
                              .init = init, .fixed_update = fixed, .render = render };
  return cw_scene_app("main.cwscene", &base);
}

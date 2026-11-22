/* backdrop: worlds of their own drawn behind the scene (cartwheel/backdrop.h).
 * The scene (main.cwscene) is a rhythm game's note highway: a lane with
 * glowing rails, notes coming down it, a red block and additive sparks. Two
 * Backdrop nodes draw behind it, each a whole world with its own camera,
 * lights and environment:
 *
 *   Stage  stage.cwscene: a dark stage in haze (volumetric HeightFog), three
 *          shadowed spot lights sweeping beams through it, two fires
 *   Sea    sea.cwscene: open water under a low sun (ProceduralSky, Water3D)
 *
 * With no test switch the two cross-fade over and over (a smooth swell, 4
 * seconds each way) while the highway plays on top.
 *
 * Test switch BD_MODE (cw_test_env, from frame 2):
 *   fire      Stage at 1, Sea at 0          sea   Sea at 1, Stage at 0
 *   mix       Stage at 1, Sea at 0.5 over it (the expected 50/50 blend)
 *   zero      both at 0                     tiny  Stage at 0.001
 *   removed   both Backdrop nodes freed (the scene alone)
 *   hidemain  Stage at 1 and the highway's nodes hidden (a mask of what the highway covers)
 *   skyhalf   a ProceduralSky in the scene, Stage at 0.5 over it
 *   split     two views side by side over Stage at 1
 * BD_MSAA=1   the scene multisampled (4x)        BD_PREPASS=1  the scene's depth prepass on
 * BD_LINEAR=1 the scene's post chain plain (LINEAR tone curve, no bloom), so
 *             pictures add up in linear light for the cross-fade check.
 *
 * Debug fields: bd.mode, bd.stage_renders, bd.sea_renders, bd.pass_ran (the
 * scene's "backdrop" pass ran last frame), bd.world_passes (passes the
 * backdrops' worlds ran last frame), bd.fog_active / bd.fog_shadowed (the
 * stage's volumetric fog: built, and its lights with shadow tiles), and the
 * opacities (bd.stage_op, bd.sea_op x 1000), and bd.gpu_ms (with CW_GPU_TIMERS=1:
 * the GPU ms of the scene's pipeline, the stage's and the sea's, and of the
 * backdrop blend pass alone; host timers, never compared). */
#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "cartwheel/backdrop.h"
#include "cartwheel/debug.h"
#include "cartwheel/environment.h"
#include "cartwheel/pipeline.h"
#include "cartwheel/render3d.h"
#include "cartwheel/scene_file.h"

static cw_node game, stage, sea, notes[3], spots[3];
static float clock_s;
static int32_t mode, stage_renders, sea_renders, pass_ran, world_passes, fog_active, fog_shadowed, stage_op, sea_op;
static bool applied;
static float gpu_ms[4]; /* GPU ms of the scene's pipeline, the stage's, the sea's, and the backdrop pass (CW_GPU_TIMERS=1, native) */
static int32_t base_renders[2], last_renders[2];

enum { M_DEMO, M_FIRE, M_SEA, M_MIX, M_ZERO, M_TINY, M_REMOVED, M_HIDEMAIN, M_SKYHALF, M_SPLIT };
static const char *const mode_names[] = { "demo", "fire", "sea", "mix", "zero", "tiny", "removed", "hidemain", "skyhalf", "split", NULL };

static void init(void *u) {
  (void)u;
  game = cw_tree_root(cw_scene_app_tree());
  game = cw_node_get(game, "Game");
  stage = cw_node_get(game, "Stage");
  sea = cw_node_get(game, "Sea");
  for (int i = 0; i < 3; i++) {
    char nm[8] = "Note0";
    nm[4] = (char)('0' + i);
    notes[i] = cw_node_get(game, nm);
  }
  cw_debug_i32("bd.mode", &mode);
  cw_debug_i32("bd.stage_renders", &stage_renders);
  cw_debug_i32("bd.sea_renders", &sea_renders);
  cw_debug_i32("bd.pass_ran", &pass_ran);
  cw_debug_i32("bd.world_passes", &world_passes);
  cw_debug_i32("bd.fog_active", &fog_active);
  cw_debug_i32("bd.fog_shadowed", &fog_shadowed);
  cw_debug_i32("bd.stage_op", &stage_op);
  cw_debug_i32("bd.sea_op", &sea_op);
  cw_debug_f32_array("bd.gpu_ms", gpu_ms, 4); /* host timers: never compared */
}

static void apply_mode(void) {
  const char *m = cw_test_env("BD_MODE");
  if (!m) return; /* (frame 2 on: a cart's env request lands then) */
  applied = true;
  base_renders[0] = cw_backdrop_renders(stage), base_renders[1] = cw_backdrop_renders(sea);
  for (int i = 0; mode_names[i]; i++)
    if (!strcmp(m, mode_names[i])) mode = i;
  const char *lin = cw_test_env("BD_LINEAR");
  if (lin && atoi(lin)) {
    cw_node post = cw_node_get(game, "Post"); /* (the PostProcess node owns the settings) */
    cw_node_set(post, "tonemap", cw_val_enum("LINEAR"));
    cw_node_set(post, "bloom", cw_val_bool(false));
  }
  cw_node post = cw_node_get(game, "Post");
  const char *ms = cw_test_env("BD_MSAA"), *pp = cw_test_env("BD_PREPASS");
  if (ms && atoi(ms)) cw_node_set(post, "msaa", cw_val_int(4));
  if (pp && atoi(pp)) cw_node_set(post, "depth_prepass", cw_val_bool(true));
  switch (mode) {
  case M_FIRE: cw_backdrop_set_opacity(stage, 1), cw_backdrop_set_opacity(sea, 0); break;
  case M_SEA: cw_backdrop_set_opacity(stage, 0), cw_backdrop_set_opacity(sea, 1); break;
  case M_MIX: cw_backdrop_set_opacity(stage, 1), cw_backdrop_set_opacity(sea, 0.5f); break;
  case M_ZERO: cw_backdrop_set_opacity(stage, 0), cw_backdrop_set_opacity(sea, 0); break;
  case M_TINY: cw_backdrop_set_opacity(stage, 0.001f), cw_backdrop_set_opacity(sea, 0); break;
  case M_REMOVED:
    cw_node_free(stage);
    cw_node_free(sea);
    stage = sea = (cw_node){ 0 };
    break;
  case M_HIDEMAIN: {
    cw_backdrop_set_opacity(stage, 1), cw_backdrop_set_opacity(sea, 0);
    static const char *const hide[] = { "Lane", "RailL", "RailR", "Target", "Note0", "Note1", "Note2", "Sparks" };
    for (int i = 0; i < 8; i++) cw_node_set(cw_node_get(game, hide[i]), "visible", cw_val_bool(false));
    break;
  }
  case M_SKYHALF: {
    cw_node sun = cw_node_new("DirectionalLight3D", "Sun");
    cw_node_add_child(game, sun);
    cw_node sky = cw_node_new("ProceduralSky", "Sky");
    cw_node_set(sky, "sun", cw_val_string("../Sun"));
    cw_node_set(sky, "time_of_day", cw_val_float(17.5f));
    cw_node_add_child(game, sky);
    cw_backdrop_set_opacity(stage, 0.5f), cw_backdrop_set_opacity(sea, 0);
    break;
  }
  case M_SPLIT: {
    cw_backdrop_set_opacity(stage, 1), cw_backdrop_set_opacity(sea, 0);
    cw_node cam = cw_node_get(game, "Camera");
    cw_node cam2 = cw_node_new("Camera3D", "Camera2");
    cw_node_set(cam2, "current", cw_val_bool(false));
    cw_node_set(cam2, "fov", cw_val_float(55));
    cw_node_add_child(game, cam2);
    cw_node3d_set_position(cam2, (t3_vec3){ 2.5f, 3.2f, 6.5f });
    cw_node3d_look_at(cam2, (t3_vec3){ 0, 0, -10 }, (t3_vec3){ 0, 1, 0 });
    cw_node cams[2] = { cam, cam2 };
    cw_pipeline_split(cw_pipeline_of(cw_scene_app_tree()), cams, 2);
    cw_pipeline_set_views_shared_post(cw_pipeline_of(cw_scene_app_tree()), true); /* as the game shell splits: one post chain */
    break;
  }
  default: break;
  }
}

static float smooth01(float x) { return x * x * (3 - 2 * x); }

static void fixed(void *u, double dt) {
  (void)u;
  clock_s += (float)dt;
  if (!applied) apply_mode();
  /* the notes come down the lane */
  for (int i = 0; i < 3; i++) {
    float z = -24 + fmodf(clock_s * 6 + (float)i * 8, 26);
    cw_node3d_set_position(notes[i], (t3_vec3){ -1.0f + (float)i, 0.25f, z });
  }
  /* #region reach_world */
  /* the stage's spot lights sweep (a world of its own: reached through the backdrop) */
  if (cw_node_valid(stage)) {
    cw_node root = cw_backdrop_root(stage);
    static const char *const nm[3] = { "SpotA", "SpotB", "SpotC" };
    for (int i = 0; i < 3; i++) {
      if (!cw_node_valid(spots[i])) spots[i] = cw_node_get(root, nm[i]);
      float s, c;
      cw_sincos(clock_s * (0.7f + 0.2f * (float)i) + (float)i * 2.1f, &s, &c);
      cw_node3d_set_rotation(spots[i], (t3_vec3){ -1.25f + 0.15f * c, 0.45f * s + 0.3f * (float)(1 - i), 0 });
    }
  }
  /* #endregion */
  /* #region crossfade */
  if (mode == M_DEMO && cw_node_valid(stage)) {
    /* a cross-fade back and forth: 4 s each way, eased, holding 2 s at each end */
    float t = fmodf(clock_s, 12) / 6;
    float k = t < 1 ? CW_CLAMP((t - 0.33f) * 1.5f, 0, 1) : 1 - CW_CLAMP((t - 1.33f) * 1.5f, 0, 1);
    cw_backdrop_set_opacity(stage, 1);
    cw_backdrop_set_opacity(sea, smooth01(k));
  }
  /* #endregion */
  /* pictures since the test switch took effect (frame 1 draws the scene file's opacities) */
  stage_renders = cw_backdrop_renders(stage) - base_renders[0];
  sea_renders = cw_backdrop_renders(sea) - base_renders[1];
  stage_op = (int32_t)lroundf(cw_backdrop_opacity(stage) * 1000);
  sea_op = (int32_t)lroundf(cw_backdrop_opacity(sea) * 1000);
  const cw_pass_stat *st;
  int n = cw_pipeline_pass_stats(cw_pipeline_of(cw_scene_app_tree()), &st);
  pass_ran = 0;
  for (int i = 0; i < n; i++)
    if (!strcmp(st[i].name, "backdrop")) pass_ran = st[i].ran;
  world_passes = 0;
  cw_node both[2] = { stage, sea };
  for (int i = 0; i < 2; i++) {
    int now = cw_backdrop_renders(both[i]), drew = now != last_renders[i];
    last_renders[i] = now;
    if (!cw_node_valid(both[i]) || !drew) continue; /* (a world not drawn last frame ran no pass; its stats are older) */
    int m = cw_pipeline_pass_stats(cw_pipeline_of(cw_backdrop_tree(both[i])), &st);
    for (int k = 0; k < m; k++) world_passes += st[k].ran;
  }
  cw_pipeline *mp = cw_pipeline_of(cw_scene_app_tree());
  gpu_ms[0] = cw_pipeline_gpu_ms(mp);
  for (int i = 0; i < 2; i++) gpu_ms[1 + i] = cw_node_valid(both[i]) ? cw_pipeline_gpu_ms(cw_pipeline_of(cw_backdrop_tree(both[i]))) : 0;
  n = cw_pipeline_pass_stats(mp, &st);
  for (int i = 0; i < n; i++)
    if (!strcmp(st[i].name, "backdrop")) gpu_ms[3] = st[i].gpu_ms_avg;
  cw_volumetric_fog_state fs = { 0 };
  if (cw_node_valid(stage)) cw_volumetric_fog_get_state(cw_backdrop_tree(stage), &fs);
  fog_active = fs.active;
  fog_shadowed = fs.shadowed_lights;
}

cw_app_desc cw_main(int argc, char **argv) {
  (void)argc;
  (void)argv;
  static cw_app_desc base = { .title = "backdrop", .width = 960, .height = 540, .debug = true, .init = init, .fixed_update = fixed };
  return cw_scene_app("main.cwscene", &base);
}

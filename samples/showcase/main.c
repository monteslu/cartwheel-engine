/* showcase: an outdoor valley with a village,
 * under the procedural sky through a day / night cycle:
 *
 *   terrain      480 m square, 12 x 12 chunks, three LOD levels each
 *                (three.c LOD by distance, skirts against cracks), vertex
 *                coloured grass / rock / road from the height and slope
 *   forest       ~1400 instanced trees (trunk + two crowns), casting shadows
 *   village      16 houses (hip roofs, chimneys), 96 windows that light up at
 *                dusk, a campfire with a shadowed flickering light
 *   lights       128 lamp posts that come on one by one as the sun sets, 64
 *                fireflies over the meadows at night, the campfire. three.c
 *                lights with at most 8 point lights: the campfire and the 7
 *                lit lamps nearest the view are real lights (a light moving
 *                to another lamp fades over a quarter second), every other
 *                lamp and the fireflies glow (emissive, bloom)
 *   characters   four skinned walkers (Khronos CesiumMan, CC-BY-4.0) circling
 *                the square on the walk cycle
 *   sky / sun    ProceduralSky driving the sun (colour, energy, direction)
 *                and its shadow map, height fog with the sun lobe
 *   effects      GPU particles: the campfire's flames, sparks and smoke,
 *                chimney smoke (soft, sorted); decals: the fire's scorch and
 *                wear on the road (CC0 art from fx_decals/make-art.mjs);
 *                a VisibilityManager culls the village's mesh nodes (BVH
 *                frustum culling, occlusion queries, primitive LOD)
 *   post         MODERN: linear HDR, GTAO, TAA, bloom, AgX, SMAA, vignette;
 *                exposure follows the sun (the eye adapts at night)
 *
 * The camera circles the village on a fixed path. The run is a schedule of
 * the time of day, SC_SEG frames a segment:
 *   seg 0   09:30  morning
 *   seg 1   17:45  golden hour, the campfire lit
 *   seg 2   18:21 -> 19:00  dusk: lamps and windows come on one by one
 *   seg 3   22:00  night: lamps, fireflies, windows, the fire
 *   seg 4+  the cycle runs from 22:00 at half an hour a second
 *
 * Debug state (compared across hosts): sc.tick, sc.seg, sc.minutes (time of
 * day x 60), sc.elev (sun elevation, centidegrees), sc.lamps (lamps on),
 * sc.lod
 * (terrain chunks at LOD 0, 1, 2), sc.trees, sc.ok (world validated),
 * sc.walk (walker 0's position x 100), sc.particles (particles alive),
 * sc.decals (decals drawn), sc.vis (meshes visible, frustum culled,
 * occlusion culled). Reported, never compared: gpu.<pass>
 * (GPU ms, 16-frame average), gpu.total, cpu.frame (wall ms between frames,
 * 32-frame average), perf.seg<k>.frame / .max / .cpu / .gpu (segment
 * averages and the worst frame, after 30 settling frames; SC_PERF). frame =
 * render end to render end, the GPU drained each frame; cpu = the part of it
 * before the drain (simulation, culling, binning, GL submission).
 *
 * showcase_perf (SC_PERF=1) renders 1920 x 1080 with 150-frame segments and
 * prints a line per segment to stderr (hosts without the debug channel's
 * state, such as wasmcart-run, report through it). */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "cartwheel/anim.h"
#include "cartwheel/debug.h"
#include "cartwheel/decals.h"
#include "cartwheel/environment.h"
#include "cartwheel/gl.h"
#include "cartwheel/particles.h"
#include "cartwheel/pipeline.h"
#include "cartwheel/platform.h"
#include "cartwheel/render3d.h"
#include "cartwheel/scene_file.h"
#include "cartwheel/visibility.h"
#include "world.h"

#ifndef SC_PERF
#define SC_PERF 0
#endif
#ifndef SC_SEG
#define SC_SEG (SC_PERF ? 150 : 60)
#endif
#define WALKERS 4

static sc_world world;
static t3_gltf *gltf[WALKERS];
static cw_node walker[WALKERS], camera, sky, post;
static int32_t particles_alive, decals_visible, vis[3];
static int32_t tick, seg = -1, minutes, elev, lamps, lod[SC_LODS], trees, ok, walk[3];
static float cpu_frame, gpu_total;

/* GPU ms per pass, by name, registered as the passes appear */
#define MAX_PASSES 40
static char pass_name[MAX_PASSES][40];
static float pass_ms[MAX_PASSES];
static int pass_count;

/* SC_PERF: per-segment averages */
#define PERF_SEGS 5
static float perf_frame[PERF_SEGS], perf_gpu[PERF_SEGS], perf_max[PERF_SEGS];
static float perf_cpu[PERF_SEGS];
static double perf_frame_sum[PERF_SEGS], perf_gpu_sum[PERF_SEGS], perf_cpu_sum[PERF_SEGS];
static int perf_n[PERF_SEGS];

static float hours_at(int32_t t, int *s_out) {
  int s = (t - 1) / SC_SEG;
  float in = (float)((t - 1) % SC_SEG) / (float)SC_SEG;
  *s_out = s < 4 ? s : 4;
  switch (s) {
  case 0: return 9.5f;
  case 1: return 17.75f;
  case 2: return 18.35f + in * 0.65f;
  case 3: return 22.0f;
  default: {
    float h = 22.0f + (float)(t - 1 - 4 * SC_SEG) / 60.0f * 0.5f;
    while (h >= 24) h -= 24;
    return h;
  }
  }
}

static void set_shadows(t3_object *o) {
  if (o->type == T3_SKINNED_MESH || o->type == T3_MESH) {
    o->cast_shadow = true;
    o->receive_shadow = true;
  }
  for (int i = 0; i < o->child_count; i++) set_shadows(o->children[i]);
}

static void load_walkers(cw_node root) {
  size_t len;
  void *glb = cw_asset_load("CesiumMan.glb", &len);
  if (!glb) { cw_error("showcase: CesiumMan.glb missing from the assets"); return; }
  cw_anim walk_clip = { 0 };
  bool have_clip = false;
  for (int i = 0; i < WALKERS; i++) {
    char name[8];
    snprintf(name, sizeof name, "W%d", i);
    walker[i] = cw_node_get(root, name);
    /* one glTF per walker: three.c binds a skeleton to its own mesh. Kept
     * for the app's lifetime (as anim_skinned). */
    gltf[i] = t3_gltf_parse(glb, len, NULL, NULL);
    if (!gltf[i] || !gltf[i]->animation_count) { cw_error("showcase: CesiumMan.glb did not parse"); break; }
    cw_node3d_set_object(cw_node_get(walker[i], "Model"), gltf[i]->scene);
    set_shadows(gltf[i]->scene);
    if (!have_clip) {
      have_clip = true;
      walk_clip = cw_anim_from_t3_clip(gltf[i]->animations[0], "Model");
      cw_anim_set_loop(walk_clip, CW_LOOP_LINEAR);
    }
    cw_node anim = cw_node_get(walker[i], "Anim");
    cw_anim_player_add(anim, "walk", walk_clip);
    cw_anim_player_play_ex(anim, "walk", 0, 0.9f + 0.07f * (float)i, false);
  }
  if (have_clip) cw_anim_release(walk_clip);
  free(glb);
}

static void place_walkers(void) {
  for (int i = 0; i < WALKERS; i++) {
    if (!gltf[i]) continue;
    /* a circle of 7.5 m round the fire, 0.63 m/s (the clip's stride) */
    float r = 7.5f + 0.6f * (float)(i & 1);
    float w = (0.63f * (0.9f + 0.07f * (float)i)) / r / 60.0f;
    float a = (float)i * CW_TAU / WALKERS + (float)tick * w, s, c;
    cw_sincos(a, &s, &c);
    float x = c * r, z = s * r;
    cw_node3d_set_position(walker[i], (t3_vec3){ x, sc_height(x, z), z });
    /* the glTF's forward is its +x: face along the circle (counter-clockwise
     * seen from above, the tangent is (-s, c)) */
    cw_node3d_set_rotation(walker[i], (t3_vec3){ 0, -a, 0 });
    if (i == 0) {
      walk[0] = (int32_t)(x * 100);
      walk[1] = (int32_t)(sc_height(x, z) * 100);
      walk[2] = (int32_t)(z * 100);
    }
  }
}

static t3_vec3 cam_focus; /* halfway from the camera to what it looks at */

static void place_camera(void) {
  float t = (float)tick;
  float a = 0.35f + t * 0.0012f, s, c;
  cw_sincos(a, &s, &c);
  float r = 30 + 8 * cw_sin(t * 0.004f);
  float x = s * r, z = c * r;
  float y = sc_height(x, z) + 7.5f + 2.5f * cw_sin(t * 0.0021f);
  cw_node3d_set_position(camera, (t3_vec3){ x, y, z });
  cw_node3d_look_at(camera, (t3_vec3){ x * 0.1f, 2.8f, z * 0.1f }, (t3_vec3){ 0, 1, 0 });
  cam_focus = (t3_vec3){ x * 0.55f, (y + 2.8f) * 0.5f, z * 0.55f };
}

static void init(void *u) {
  (void)u;
  cw_tree *t = cw_scene_app_tree();
  cw_node root = cw_node_get(cw_tree_root(t), "World");
  camera = cw_node_get(root, "Camera");
  sky = cw_node_get(root, "Sky");
  post = cw_node_get(root, "Post");
  ok = sc_world_build(&world, root);
  sc_world_build_fx(&world, root);
  trees = world.tree_count;
  load_walkers(root);
  place_walkers();
  place_camera();
  cw_debug_i32("sc.tick", &tick);
  cw_debug_i32("sc.seg", &seg);
  cw_debug_i32("sc.minutes", &minutes);
  cw_debug_i32("sc.elev", &elev);
  cw_debug_i32("sc.lamps", &lamps);
  cw_debug_field("sc.lod", lod, CW_DBG_I32, SC_LODS);
  cw_debug_i32("sc.trees", &trees);
  cw_debug_i32("sc.ok", &ok);
  cw_debug_field("sc.walk", walk, CW_DBG_I32, 3);
  cw_debug_i32("sc.particles", &particles_alive);
  cw_debug_i32("sc.decals", &decals_visible);
  cw_debug_field("sc.vis", vis, CW_DBG_I32, 3);
  cw_debug_f32("gpu.total", &gpu_total);
  cw_debug_f32("cpu.frame", &cpu_frame);
  if (SC_PERF) {
    static char names[PERF_SEGS][4][24];
    for (int s = 0; s < PERF_SEGS; s++) {
      snprintf(names[s][0], sizeof names[s][0], "perf.seg%d.frame", s);
      snprintf(names[s][1], sizeof names[s][1], "perf.seg%d.gpu", s);
      snprintf(names[s][2], sizeof names[s][2], "perf.seg%d.max", s);
      cw_debug_f32(names[s][0], &perf_frame[s]);
      cw_debug_f32(names[s][1], &perf_gpu[s]);
      cw_debug_f32(names[s][2], &perf_max[s]);
      snprintf(names[s][3], sizeof names[s][3], "perf.seg%d.cpu", s);
      cw_debug_f32(names[s][3], &perf_cpu[s]);
    }
  }
}

static void fixed_update(void *u, double dt) {
  (void)u;
  (void)dt;
  tick++;
  int s;
  float h = hours_at(tick, &s);
  seg = s;
  cw_node_set(sky, "time_of_day", cw_val_float(h));
  cw_sky_state st;
  cw_sky_get_state(cw_scene_app_tree(), &st);
  /* the sun's elevation from last tick's sky update drives the lamps */
  sc_world_tick(&world, (uint64_t)tick, st.sun_elevation);
  /* eye adaptation: open up about 1.6 stops at night */
  float night = 1.0f - cw_smoothstep(-8, 10, st.sun_elevation);
  cw_node_set(post, "exposure_ev", cw_val_float(1.6f * night));
  place_walkers();
  place_camera();
  sc_world_lamp_lights(&world, cam_focus);
}

static void perf_sample(float d, float cpu) {
  int in = (tick - 1) % SC_SEG;
  if (in >= 30) { /* after the segment's switch settles */
    perf_frame_sum[seg] += d;
    perf_gpu_sum[seg] += gpu_total;
    perf_cpu_sum[seg] += cpu;
    perf_cpu[seg] = (float)(perf_cpu_sum[seg] / (perf_n[seg] + 1));
    perf_n[seg]++;
    perf_frame[seg] = (float)(perf_frame_sum[seg] / perf_n[seg]);
    perf_gpu[seg] = (float)(perf_gpu_sum[seg] / perf_n[seg]);
    if (d > perf_max[seg]) perf_max[seg] = d;
  }
  if (in == SC_SEG - 1) {
    /* one line a segment: hosts without the debug state report through it */
    char line[1200];
    int o = snprintf(line, sizeof line, "showcase-perf seg=%d frame_ms=%.3f max_ms=%.3f cpu_ms=%.3f gpu_ms=%.3f lamps=%d",
                     seg, perf_frame[seg], perf_max[seg], perf_cpu[seg], perf_gpu[seg], lamps);
    for (int k = 0; k < pass_count && o < (int)sizeof line - 48; k++)
      if (pass_ms[k] >= 0) o += snprintf(line + o, sizeof line - (size_t)o, " %s=%.3f", pass_name[k] + 4, pass_ms[k]);
    fprintf(stderr, "%s\n", line);
  }
}

static double wall_ms(void) {
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (double)ts.tv_sec * 1000.0 + (double)ts.tv_nsec * 1e-6;
}

static void render(void *u, double alpha) {
  (void)u;
  (void)alpha;
  cw_tree *t = cw_scene_app_tree();
  cw_sky_state st;
  cw_sky_get_state(t, &st);
  minutes = (int32_t)(st.time_of_day * 60 + 0.5f);
  elev = (int32_t)(st.sun_elevation * 100 + (st.sun_elevation >= 0 ? 0.5f : -0.5f));
  lamps = sc_world_lamps_on(&world);
  sc_world_lod_counts(&world, lod);
  cw_particles_stats pst;
  cw_particles_get_stats(t, &pst);
  particles_alive = pst.alive;
  cw_decal_stats dst;
  cw_decals_get_stats(t, &dst);
  decals_visible = dst.visible;
  cw_visibility_stats vst;
  cw_visibility_get_stats(t, &vst);
  vis[0] = vst.visible;
  vis[1] = vst.frustum_culled;
  vis[2] = vst.occlusion_culled;

  const cw_pass_stat *ps;
  int n = cw_pipeline_pass_stats(cw_pipeline_of(t), &ps);
  for (int i = 0; i < n; i++) {
    int k = 0;
    while (k < pass_count && strcmp(pass_name[k] + 4, ps[i].name)) k++;
    if (k == pass_count && pass_count < MAX_PASSES) {
      snprintf(pass_name[k], sizeof pass_name[k], "gpu.%s", ps[i].name);
      pass_ms[k] = -1;
      cw_debug_f32(pass_name[k], &pass_ms[k]);
      pass_count++;
    }
    if (k < pass_count) pass_ms[k] = ps[i].gpu_ms_avg;
  }
  gpu_total = cw_pipeline_gpu_ms(cw_pipeline_of(t));

  /* frame time: render end to render end. SC_PERF builds wait for the GPU
   * after every frame (a 1-pixel read), so this is the whole frame's work,
   * CPU and GPU, not a pipelined submission rate */
  static double last;
  double submitted = wall_ms();
  if (SC_PERF) {
    uint8_t px[4];
    glReadPixels(0, 0, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, px);
  }
  double now = wall_ms();
  if (last > 0) {
    float d = (float)(now - last);
    cpu_frame = cpu_frame > 0 ? cpu_frame + (d - cpu_frame) / 32 : d;
    if (SC_PERF && seg >= 0 && seg < PERF_SEGS) perf_sample(d, (float)(submitted - last));
  }
  last = now;
}

/* tick 0 is the scene's state before the first fixed step; the runner
 * renders after each frame's ticks */
cw_app_desc cw_main(int argc, char **argv) {
  (void)argc;
  (void)argv;
  static cw_app_desc base = { .title = SC_PERF ? "showcase_perf" : "showcase", .width = SC_PERF ? 1920 : 640,
                              .height = SC_PERF ? 1080 : 360, .debug = true, .init = init, .fixed_update = fixed_update,
                              .render = render };
  return cw_scene_app("main.cwscene", &base);
}

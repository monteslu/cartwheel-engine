/* post_stack: the render pipeline's post stack on one static scene (an
 * enclosed room for AO, bright emissives for bloom, a generated .cube LUT).
 *
 * The run is a schedule: each segment sets the PostProcess node's
 * properties (the same reflection path a scene file or the debug channel
 * uses) for a number of frames, so one cart shows every effect in turn and
 * each gate captures the end of one segment, with the baseline segment
 * (AgX, nothing else) as its "effect off" control. The last segments show a
 * bar spinning in front of the camera (hidden in every other segment): under
 * TAA, and as the velocity buffer (the bar's own motion over the camera's). Then
 * automatic exposure: the room, its lamps turned up 30 times, and back down. The TAA segment
 * also measures convergence: a 64x64 crop over edges is read back every
 * frame and compared with the one 8 frames (one jitter cycle) earlier.
 *
 * Debug state: post.seg (segment index), post.passes (bit i = the i-th of
 * pass_names ran last frame), post.fused (bit i = it ran inside another
 * pass: post fusion; its GPU time is that pass's), pipe.float (RGBA16F renderable),
 * taa.delta (mean abs change per channel of the crop over one jitter cycle,
 * x1000), taa.max (largest change), taa.converged (mean < 0.1, max <= 4),
 * and gpu.<pass> (GPU ms, averaged; reported, never compared).
 *
 * POST_PERF builds hold the full-quality segment at 1280x720 from frame 1
 * (tests/pipeline-perf.mjs reads its GPU timings). */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cartwheel/debug.h"
#include "cartwheel/gpu.h"
#include "cartwheel/pipeline.h"
#include "cartwheel/platform.h"
#include "cartwheel/scene_file.h"
#include "cartwheel/ui.h"

#ifndef POST_PERF
#define POST_PERF 0
#endif

typedef struct seg { const char *name; int frames; const char *props; } seg;
/* props: "name=value;..." in the scene-file value syntax */
static const seg segs[] = {
  { "base", 20, "" },
  { "bloom", 20, "bloom=true;bloom_intensity=0.3;bloom_threshold=1.5" },
  { "ssao", 20, "ssao=true;ssao_radius=0.7" },
  { "aces", 20, "tonemap=ACES_FILMIC" },
  { "neutral", 20, "tonemap=NEUTRAL" },
  { "grade", 20, "grade=true" },
  { "fxaa", 20, "aa=FXAA" },
  { "smaa", 20, "aa=SMAA" },
  { "vignette", 20, "vignette=true;vignette_intensity=0.5" },
  { "grain", 20, "grain=true;grain_intensity=0.06" },
  { "ca", 20, "chromatic_aberration=true;ca_intensity=0.006" },
  { "msaa", 20, "msaa=4" },
  { "taa", 48, "taa=true" },
  { "full", 30, "msaa=4;taa=true;bloom=true;bloom_intensity=0.06;ssao=true;ssao_radius=0.7;grade=true;grade_intensity=0.8;"
                "vignette=true;chromatic_aberration=true;ca_intensity=0.002;grain=true;grain_intensity=0.03" },
  { "prepass", 20, "depth_prepass=true;bloom=true;ssao=true;ssao_radius=0.7" },
  { "exposure", 20, "exposure_ev=1;tonemap=REINHARD" },
  { "aces", 20, "tonemap=ACES_FILMIC" },
  { "aces_post", 20, "tonemap=ACES_FILMIC;bloom=true;bloom_intensity=0.15;aa=FXAA" },
  { "view_normals", 10, "debug_view=NORMALS" },
  { "view_depth", 10, "debug_view=DEPTH" },
  { "view_ao", 10, "ssao=true;ssao_radius=0.7;debug_view=AO" },
  { "dof", 20, "dof=true;dof_focus_distance=4.5;dof_focus_range=1;dof_blur=10;dof_near_blur=0.6" },
  { "motion_plain", 24, "" },
  { "motion_taa", 24, "taa=true" },
  { "motion_velocity", 12, "taa=true;debug_view=VELOCITY" },
  /* automatic exposure: adapted to the room, then the room's lamps 30 times as bright (the picture swells
   * back over about a second), then the lamps as before (it comes back more slowly) */
  { "auto_room", 30, "auto_exposure=true;auto_exposure_min_ev=-8;auto_exposure_max_ev=8" },
  { "auto_bright", 90, "auto_exposure=true;auto_exposure_min_ev=-8;auto_exposure_max_ev=8" },
  { "auto_dim", 150, "auto_exposure=true;auto_exposure_min_ev=-8;auto_exposure_max_ev=8" },
  /* motion blur on the spinning bar (its own motion: the camera is still) */
  { "motion_blur", 24, "motion_blur=true" },
  /* star rays on the lamp and the light strip */
  { "glints", 20, "glints=true;glint_threshold=3" },
};
#define NSEG ((int)(sizeof segs / sizeof segs[0]))

static const char *const pass_names[] = { "depth_prepass", "scene", "velocity", "ssao", "taa", "bloom", "tonemap", "grade",
                                          "aa", "lens", "app", "dof", "exposure", "motion_blur", "glints" };
static int32_t seg_index = -1, passes_mask, fused_mask, float_ok, taa_delta, taa_max, taa_converged;
static uint32_t frame_no;
static float gpu_ms[24];
/* the last 9 crops: a converged history repeats with the 8-frame jitter
 * cycle, so frame n is compared with frame n - 8 */
static uint8_t crops[9][64 * 64 * 4];
static int crop_count;

static cw_node post_node(void) { return cw_node_get(cw_tree_root(cw_scene_app_tree()), "Room/Post"); }

/* reset the node to the baseline, then apply a segment's properties */
static void apply_props(const char *props) {
  cw_node n = post_node();
  static const char *const base =
      "hdr=true;tonemap=AGX;exposure=1;exposure_ev=0;msaa=0;depth_prepass=false;bloom=false;"
      "bloom_intensity=0.05;ssao=false;ssao_radius=0.5;taa=false;aa=NONE;grade=false;grade_intensity=1;vignette=false;"
      "vignette_intensity=0.35;grain=false;grain_intensity=0.04;chromatic_aberration=false;ca_intensity=0.004;"
      "debug_view=NONE;bloom_threshold=0;dof=false;auto_exposure=false;auto_exposure_min_ev=-3;auto_exposure_max_ev=3;motion_blur=false;glints=false";
  for (int pass = 0; pass < 2; pass++) {
    const char *s = pass ? props : base;
    while (*s) {
      const char *e = strchr(s, ';');
      size_t len = e ? (size_t)(e - s) : strlen(s);
      char kv[128];
      memcpy(kv, s, CW_MIN(len, sizeof kv - 1));
      kv[CW_MIN(len, sizeof kv - 1)] = 0;
      char *eq = strchr(kv, '=');
      if (eq) {
        *eq = 0;
        const cw_prop *pr = cw_node_find_prop(n, kv, NULL);
        cw_value v;
        if (pr && pr->type == CW_T_ENUM) cw_node_set(n, kv, cw_val_enum(eq + 1));
        else if (pr && cw_value_parse(eq + 1, pr->type, &v)) cw_node_set(n, kv, v);
        else cw_error("post_stack: bad property %s=%s", kv, eq + 1);
      }
      s += len + (e ? 1 : 0);
    }
  }
}

static void init(void *u) {
  (void)u;
  cw_debug_i32("post.seg", &seg_index);
  cw_debug_i32("post.passes", &passes_mask);
  cw_debug_i32("post.fused", &fused_mask);
  cw_debug_i32("pipe.float", &float_ok);
  cw_debug_i32("taa.delta", &taa_delta);
  cw_debug_i32("taa.max", &taa_max);
  cw_debug_i32("taa.converged", &taa_converged);
  static char keys[CW_ARRAY_LEN(pass_names)][32];
  for (size_t i = 0; i < CW_ARRAY_LEN(pass_names); i++) {
    snprintf(keys[i], sizeof keys[i], "gpu.%s", pass_names[i]);
    cw_debug_f32(keys[i], &gpu_ms[i]);
  }
}

static void update(void *u, double dt) {
  (void)u;
  (void)dt;
  frame_no++;
  int k = 0;
  if (POST_PERF) k = 13;
  else {
    int start = 0;
    for (k = 0; k < NSEG - 1; k++) {
      if ((int)frame_no <= start + segs[k].frames) break;
      start += segs[k].frames;
    }
  }
  /* the spinning bar: its angle from the frame number, so every host poses it alike */
  bool motion = !strncmp(segs[k].name, "motion", 6);
  /* POST_REDUCED_MOTION=1 (the motion blur gate's control): the player's reduced motion setting, on */
  const char *rm = cw_test_env("POST_REDUCED_MOTION");
  cw_ui_set_reduced_motion(rm && *rm == '1');
  cw_node spinner = cw_node_get(cw_tree_root(cw_scene_app_tree()), "Room/Spinner");
  const char *mref = cw_test_env("POST_MOTION_REF"); /* 2: the same frames without the bar (the measurement's mask) */
  cw_node_set(spinner, "visible", cw_val_bool(motion && !(mref && *mref == '2')));
  if (motion) cw_node3d_set_rotation(spinner, (t3_vec3){ 0, 0, (float)frame_no * 0.21f });
  if (k != seg_index) {
    /* the lamps' brightness for the automatic exposure segments */
    static const char *const lamps[] = { "Room/LampLight", "Room/Fill", "Room/Spot" };
    static const float energy[] = { 14, 6, 30 };
    float gain = !strcmp(segs[k].name, "auto_bright") ? 30.0f : 1.0f;
    for (int i = 0; i < 3; i++) cw_node_set(cw_node_get(cw_tree_root(cw_scene_app_tree()), lamps[i]), "energy", cw_val_float(energy[i] * gain));
    seg_index = k;
    /* POST_MOTION_REF=1 (tests/motion-vectors.mjs): the TAA motion segment without TAA, the same poses: with
     * CW_RENDER_SCALE it is the supersampled picture the TAA frame is measured against */
    const char *ref = cw_test_env("POST_MOTION_REF");
    apply_props(ref && *ref != '0' && !strcmp(segs[k].name, "motion_taa") ? "" : segs[k].props);
    crop_count = 0;
    taa_delta = 0;
    taa_converged = 0;
  }
}

/* the app's overlay pass: the output is bound and complete */
static void taa_crop(const uint8_t *crop_cur);

static void render(void *u, double alpha) {
  (void)u;
  (void)alpha;
  cw_pipeline *p = cw_pipeline_of(cw_scene_app_tree());
  float_ok = cw_pipeline_caps_get()->float_render;
  const cw_pass_stat *st;
  int n = cw_pipeline_pass_stats(p, &st);
  /* by name: other modules add passes of their own */
  passes_mask = fused_mask = 0;
  for (int k = 0; k < (int)CW_ARRAY_LEN(pass_names); k++) {
    gpu_ms[k] = -1;
    for (int i = 0; i < n; i++)
      if (!strcmp(st[i].name, pass_names[k])) {
        if (st[i].ran) passes_mask |= 1 << k;
        if (st[i].fused) fused_mask |= 1 << k;
        gpu_ms[k] = st[i].gpu_ms_avg;
      }
  }
  /* window reads (both backends): one crop asked every frame of the taa
   * segment, taken in order as they arrive (a frame or more later on
   * WebGPU); a read from an earlier segment is dropped */
  static uint32_t q_ticket[8];
  static int q_seg[8], q_n;
  bool taa = strcmp(segs[seg_index].name, "taa") == 0;
  while (q_n > 0) {
    uint8_t *crop_cur = crops[crop_count % 9];
    int got = cw_gpu_read_result(q_ticket[0], crop_cur);
    if (got == 0) break;
    int from = q_seg[0];
    q_n--;
    memmove(q_ticket, q_ticket + 1, sizeof q_ticket[0] * (size_t)q_n);
    memmove(q_seg, q_seg + 1, sizeof q_seg[0] * (size_t)q_n);
    if (got < 0 || from != seg_index) continue;
    taa_crop(crop_cur);
  }
  if (taa && q_n < 8) {
    int w, h;
    cw_drawable_size(&w, &h);
    /* the ball's silhouette and the crates' edges: where jitter shows */
    uint32_t t = cw_gpu_read_window(w * 260 / 640, h - h * 150 / 360 - 64, 64, 64);
    if (t) q_ticket[q_n] = t, q_seg[q_n++] = seg_index;
  }
}

static void taa_crop(const uint8_t *crop_cur) {
  const uint8_t *crop_prev = crops[(crop_count + 1) % 9]; /* 8 frames ago */
  if (++crop_count > 8) {
    long sum = 0;
    int mx = 0;
    for (int i = 0; i < 64 * 64 * 4; i++)
      if ((i & 3) != 3) {
        int d = abs((int)crop_cur[i] - (int)crop_prev[i]);
        sum += d;
        mx = d > mx ? d : mx;
      }
    taa_delta = (int32_t)(sum * 1000 / (64 * 64 * 3));
    taa_max = mx;
    taa_converged = taa_delta < 100 && taa_max <= 4;
  }
}

cw_app_desc cw_main(int argc, char **argv) {
  (void)argc;
  (void)argv;
  static cw_app_desc base = {
    .title = "post_stack", .width = POST_PERF ? 1280 : 640, .height = POST_PERF ? 720 : 360, .debug = true,
    .render_scale = POST_PERF ? 1 : 0, /* the perf report times 1280x720 */
    .init = init, .update = update, .render = render,
  };
  return cw_scene_app("main.cwscene", &base);
}

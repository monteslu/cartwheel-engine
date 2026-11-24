/* looks: the post looks of cartwheel/look.h on a small lit scene (a sky, a sun with shadows, coloured shapes, two
 * glowing strips): characters, half blocks, ordered and diffused one-bit dithering, a shader of the sample's own, and
 * blends between any two of them.
 *
 * Without switches it tours them, a few seconds each, with a blend into each. Test switches (cw_test_env, read every
 * frame, so they reach a cart from its second frame):
 *   LOOK_A, LOOK_B   the pair: none | ascii | halfblock | palette | levels | bayer | bluenoise | diffuse | duotone | custom
 *   LOOK_T           the blend weight, 0..1 (0)
 *   LOOK_HOW         fade | dissolve | wipe (fade); LOOK_SOFT (the edge's softness), LOOK_ANGLE (the wipe's, radians)
 *   LOOK_RAMP=N      the weight from 0 to 1 over N frames from frame LOOK_RAMP_AT (60), then held at 1
 *   LOOK_FLICKER=N   the weight jumps between 0 and 1 every N frames (the flash check's control)
 *   LOOK_OFF_AT=N    no look from frame N on (the pair set before it is cleared)
 *   LOOK_PLAIN=1     no bloom and the tone curve NONE: every pixel is what the look wrote (the property gates)
 *   LOOK_SPLIT=1     two views side by side with the shared post chain (one pass unless CW_TEST_VIEWS_ARRAY=0)
 *   LOOK_BOXES=1     a HUD panel over a dimmed box, a hint over a clean one
 *   LOOK_ORBIT=1     the camera orbits (still otherwise)
 *   LOOK_TAA=1       TAA on
 *   LOOK_STAGE=display   the looks at the DISPLAY stage (read when a look is made: set it from the start)
 *   LOOK_GLYPHS=s    the ASCII look's glyph set
 *   LOOK_MEASURE=1   cw_pipeline_measure_looks (look.lum_a, look.lum_b, look.lum_ok)
 *   LOOK_FLAT=S      a flat card (black, no specular, emissive: one colour) in front of a bright one, sliding right S
 *                    world units a tick (0: still); its colour LOOK_FLAT_GREY display steps (0.53: between the
 *                    palette's grey and silver, nearer the grey), the bright card's 0.8 (read when the cards are made:
 *                    set them from the start)
 *
 * The debug request "look A [B [T]]" sets the pair and weight over the switches.
 *
 * Debug state: look.tick, look.weight, look.split, look.array (the split screen drew in one pass), look.pass_ran (the
 * "looks" pass ran last frame), look.lum_*, and the engine's look.ran, look.kind_a / _b, look.t, look.cells.
 * LOOKS_PERF builds (looks_perf) render at 1920x1080 and publish look.gpu_ms, the "looks" pass's GPU time (natively,
 * CW_GPU_TIMERS=1). */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cartwheel/debug.h"
#include "cartwheel/gpu.h"
#include "cartwheel/look.h"
#include "cartwheel/pipeline.h"
#include "cartwheel/platform.h"
#include "cartwheel/render2d.h"
#include "cartwheel/scene_file.h"
#include "cartwheel/ui.h"

#ifndef LOOKS_PERF
#define LOOKS_PERF 0
#endif

static int32_t tick, lum_ok, split_on, array_used, pass_ran;
static float weight, lum_a = -1, lum_b = -1, gpu_ms = -1;
static cw_node cams[2];

/* a 16 colour palette (the classic terminal one), linear */
static float palette16[16 * 3];
static void make_palette(void) {
  static const unsigned char srgb[16][3] = { { 0, 0, 0 },       { 128, 0, 0 },   { 0, 128, 0 },   { 128, 128, 0 },
                                             { 0, 0, 128 },     { 128, 0, 128 }, { 0, 128, 128 }, { 192, 192, 192 },
                                             { 128, 128, 128 }, { 255, 0, 0 },   { 0, 255, 0 },   { 255, 255, 0 },
                                             { 0, 0, 255 },     { 255, 0, 255 }, { 0, 255, 255 }, { 255, 255, 255 } };
  for (int i = 0; i < 16 * 3; i++) {
    float x = srgb[i / 3][i % 3] / 255.0f;
    palette16[i] = x <= 0.04045f ? x / 12.92f : powf((x + 0.055f) / 1.055f, 2.4f);
  }
}

/* the sample's own look: the scene's colour, part desaturated, a tint, every other pair of lines darker */
static const char *CUSTOM_GLSL =
    "vec3 look(vec2 px) {\n"
    "  vec3 c = cw_look_scene(px);\n"
    "  float l = dot(c, vec3(0.2126, 0.7152, 0.0722));\n"
    "  float scan = 0.7 + 0.3 * step(1.0, mod(px.y / (2.0 * cw_look_scale()), 2.0));\n"
    "  vec4 k = cw_look_param(0);\n"
    "  return mix(vec3(l), c, k.x) * k.yzw * scan;\n"
    "}\n";
static const char *CUSTOM_WGSL =
    "fn look(px: vec2f) -> vec3f {\n"
    "  let c = cw_look_scene(px);\n"
    "  let l = dot(c, vec3f(0.2126, 0.7152, 0.0722));\n"
    "  let scan = 0.7 + 0.3 * step(1.0, (px.y / (2.0 * cw_look_scale())) % 2.0);\n"
    "  let k = cw_look_param(0);\n"
    "  return mix(vec3f(l), c, k.x) * k.yzw * scan;\n"
    "}\n";

enum { K_NONE, K_ASCII, K_HALFBLOCK, K_PALETTE, K_LEVELS, K_BAYER, K_BLUENOISE, K_DIFFUSE, K_DUOTONE, K_CUSTOM, K_COUNT };
static const char *const KNAMES[K_COUNT] = { "none", "ascii", "halfblock", "palette", "levels", "bayer", "bluenoise", "diffuse", "duotone", "custom" };
static cw_look looks[K_COUNT];
static char glyphs_now[96];

/* the debug request "look A [B [T]]": the pair and weight from now on, over the switches (timing variants) */
static char cmd_a[16], cmd_b[16];
static float cmd_t = -1;
static void cmd_look(int argc, const char **argv, cw_reply *r) {
  snprintf(cmd_a, sizeof cmd_a, "%s", argc > 1 ? argv[1] : "");
  snprintf(cmd_b, sizeof cmd_b, "%s", argc > 2 ? argv[2] : "none");
  cmd_t = argc > 3 ? (float)atof(argv[3]) : 0;
  cw_reply_printf(r, "{\"a\":\"%s\",\"b\":\"%s\",\"t\":%g}", cmd_a, cmd_b, (double)cmd_t);
}

static int kind_of(const char *s) {
  if (!s) return -1;
  for (int i = 0; i < K_COUNT; i++)
    if (!strcmp(s, KNAMES[i])) return i;
  return -1;
}

static float envf(const char *name, float def);
static cw_look_desc desc_of(int k) {
  cw_look_desc d = { 0 };
  switch (k) {
  case K_ASCII:
    d = cw_look_default(CW_LOOK_ASCII);
    if (glyphs_now[0]) d.ascii.glyphs = glyphs_now;
    d.ascii.hold = envf("LOOK_HOLD", d.ascii.hold);
    break;
  case K_HALFBLOCK: d = cw_look_default(CW_LOOK_HALFBLOCK); break;
  case K_PALETTE:
    d = cw_look_default(CW_LOOK_HALFBLOCK);
    d.halfblock.rows = 45;
    d.halfblock.palette = palette16;
    d.halfblock.palette_count = 16;
    break;
  case K_LEVELS:
    d = cw_look_default(CW_LOOK_HALFBLOCK);
    d.halfblock.levels = 4;
    break;
  case K_BAYER: d = cw_look_default(CW_LOOK_DITHER_ORDERED); break;
  case K_BLUENOISE:
    d = cw_look_default(CW_LOOK_DITHER_ORDERED);
    d.dither.pattern = CW_DITHER_BLUE_NOISE;
    break;
  case K_DIFFUSE: d = cw_look_default(CW_LOOK_DITHER_DIFFUSE); break;
  case K_DUOTONE:
    d = cw_look_default(CW_LOOK_DITHER_DIFFUSE);
    d.dither.pixel = 3;
    d.dither.dark[0] = 0.02f, d.dither.dark[1] = 0.03f, d.dither.dark[2] = 0.12f;
    d.dither.light[0] = 1.0f, d.dither.light[1] = 0.62f, d.dither.light[2] = 0.18f;
    break;
  case K_CUSTOM:
    d = cw_look_default(CW_LOOK_CUSTOM);
    d.custom.glsl = CUSTOM_GLSL, d.custom.wgsl = CUSTOM_WGSL;
    d.custom.params[0] = 0.35f, d.custom.params[1] = 0.6f, d.custom.params[2] = 1.0f, d.custom.params[3] = 0.75f;
    break;
  default: break;
  }
  const char *st = cw_test_env("LOOK_STAGE");
  if (st && !strcmp(st, "display")) d.stage = CW_LOOK_STAGE_DISPLAY;
  return d;
}

static cw_look look_of(int k) {
  if (k <= K_NONE || k >= K_COUNT) return (cw_look){ 0 };
  if (CW_HANDLE_IS_NULL(looks[k])) {
    cw_look_desc d = desc_of(k);
    looks[k] = cw_look_new(&d);
  }
  return looks[k];
}

static float envf(const char *name, float def) {
  const char *e = cw_test_env(name);
  return e && *e ? (float)atof(e) : def;
}
static int envi(const char *name, int def) {
  const char *e = cw_test_env(name);
  return e && *e ? atoi(e) : def;
}

/* the tour: each stop a pair and a weight ramp */
typedef struct stop { int a, b, frames; cw_look_blend how; } stop;
static const stop TOUR[] = {
  { K_NONE, K_NONE, 90, CW_LOOK_FADE },        { K_NONE, K_ASCII, 150, CW_LOOK_FADE },
  { K_ASCII, K_HALFBLOCK, 150, CW_LOOK_WIPE },  { K_HALFBLOCK, K_PALETTE, 150, CW_LOOK_DISSOLVE },
  { K_PALETTE, K_BAYER, 150, CW_LOOK_FADE },    { K_BAYER, K_DIFFUSE, 150, CW_LOOK_WIPE },
  { K_DIFFUSE, K_DUOTONE, 150, CW_LOOK_DISSOLVE }, { K_DUOTONE, K_CUSTOM, 150, CW_LOOK_FADE },
  { K_CUSTOM, K_NONE, 150, CW_LOOK_FADE },
};

static bool plain_applied, taa_applied, split_applied, measure_applied;
static cw_node flat_card;

/* a grey of display steps d (look.h's display curve: d = (c / (1 + c))^(1 / 2.2)) in linear light */
static float undisp(float d) {
  float y = powf(d, 2.2f);
  return y / (1.0f - y);
}
static cw_node add_card(cw_node world, const char *name, float x, float y, float z, float w, float h, float grey) {
  cw_node n = cw_node_new("MeshInstance3D", name);
  cw_node_add_child(world, n);
  cw_node_set(n, "size", cw_val_vec3(w, h, 0.05f));
  cw_node_set(n, "color", cw_val_color(0, 0, 0, 1));
  cw_node_set(n, "roughness", cw_val_float(1));
  cw_node_set(n, "specular_intensity", cw_val_float(0)); /* (no highlight, no reflection: the emissive alone) */
  cw_node_set(n, "emissive", cw_val_color(grey, grey, grey, 1));
  cw_node_set(n, "cast_shadow", cw_val_bool(false));
  cw_node_set(n, "receive_shadow", cw_val_bool(false));
  cw_node3d_set_position(n, (t3_vec3){ x, y, z });
  return n;
}

static void apply(void) {
  cw_tree *tree = cw_scene_app_tree();
  cw_pipeline *p = cw_pipeline_of(tree);
  cw_node world = cw_node_get(cw_tree_root(tree), "World");
  cw_node post = cw_node_get(world, "Post");
  bool plain = envi("LOOK_PLAIN", 0) != 0;
  if (plain != plain_applied) {
    plain_applied = plain;
    cw_node_set(post, "bloom", cw_val_bool(!plain));
    cw_node_set(post, "tonemap", cw_val_enum(plain ? "NONE" : "AGX"));
  }
  bool taa = envi("LOOK_TAA", 0) != 0;
  if (taa != taa_applied) taa_applied = taa, cw_node_set(post, "taa", cw_val_bool(taa));
  bool split = envi("LOOK_SPLIT", 0) != 0;
  if (split != split_applied) {
    split_applied = split;
    split_on = split;
    if (split) {
      cw_pipeline_split(p, cams, 2);
      cw_pipeline_set_views_shared_post(p, true);
    } else cw_pipeline_set_views(p, NULL, 0);
  }
  if (cw_test_env("LOOK_FLAT") && CW_HANDLE_IS_NULL(flat_card)) {
    add_card(world, "FlatBack", 0, 1.8f, 2, 14, 6, undisp(0.8f));
    flat_card = add_card(world, "Flat", -3, 1.8f, 4, 4, 2.5f, undisp(envf("LOOK_FLAT_GREY", 0.53f)));
  }
  if (!CW_HANDLE_IS_NULL(flat_card)) cw_node3d_set_position(flat_card, (t3_vec3){ -3 + envf("LOOK_FLAT", 0) * (float)tick, 1.8f, 4 });
  bool meas = envi("LOOK_MEASURE", 0) != 0;
  if (meas != measure_applied) measure_applied = meas, cw_pipeline_measure_looks(p, meas);
  const char *gl = cw_test_env("LOOK_GLYPHS");
  if (gl && strcmp(gl, glyphs_now)) {
    snprintf(glyphs_now, sizeof glyphs_now, "%s", gl);
    if (!CW_HANDLE_IS_NULL(looks[K_ASCII])) {
      cw_look_desc d = desc_of(K_ASCII);
      cw_look_set(looks[K_ASCII], &d);
    }
  }

  /* the pair and the weight */
  int ka = kind_of(cw_test_env("LOOK_A")), kb = kind_of(cw_test_env("LOOK_B"));
  cw_look_mix how = { 0 };
  const char *hw = cw_test_env("LOOK_HOW");
  how.blend = hw && !strcmp(hw, "wipe") ? CW_LOOK_WIPE : hw && !strcmp(hw, "dissolve") ? CW_LOOK_DISSOLVE : CW_LOOK_FADE;
  how.softness = envf("LOOK_SOFT", 0);
  how.angle = envf("LOOK_ANGLE", 0);
  float t;
  if (cmd_a[0]) ka = kind_of(cmd_a), kb = kind_of(cmd_b);
  if (ka < 0 && kb < 0 && !LOOKS_PERF) {
    /* the tour */
    int total = 0;
    for (size_t i = 0; i < CW_ARRAY_LEN(TOUR); i++) total += TOUR[i].frames;
    int f = tick % total, i = 0;
    while (f >= TOUR[i].frames) f -= TOUR[i++].frames;
    ka = TOUR[i].a, kb = TOUR[i].b, how.blend = TOUR[i].how, how.softness = how.blend == CW_LOOK_FADE ? 0 : 0.08f;
    t = CW_CLAMP((float)(f - 60) / 45.0f, 0.0f, 1.0f); /* a hold, then a blend of three quarters of a second */
    t = t * t * (3 - 2 * t);
  } else {
    if (ka < 0) ka = K_NONE;
    if (kb < 0) kb = K_NONE;
    t = CW_CLAMP(cmd_a[0] ? cmd_t : envf("LOOK_T", 0), 0.0f, 1.0f);
    int ramp = envi("LOOK_RAMP", 0), at = envi("LOOK_RAMP_AT", 60), flick = envi("LOOK_FLICKER", 0), off = envi("LOOK_OFF_AT", 0);
    if (off > 0 && tick >= off) ka = kb = K_NONE;
    if (ramp > 0) t = CW_CLAMP((float)(tick - at) / (float)ramp, 0.0f, 1.0f);
    if (flick > 0) t = tick >= at ? (float)(((tick - at) / flick) & 1) : 0.0f;
  }
  weight = t;
  cw_pipeline_set_looks(p, look_of(ka), look_of(kb), t, &how);

  /* the HUD boxes: the score panel (dimmed under it) and the hint (the plain scene under it) */
  if (envi("LOOK_BOXES", 0))
    cw_pipeline_set_look_boxes(p, (cw_look_box[]){ { 0.03f, 0.05f, 0.34f, 0.13f, 0.8f, false },
                                                   { 0.62f, 0.8f, 0.34f, 0.12f, 0, true } }, 2);
  else cw_pipeline_set_look_boxes(p, NULL, 0);
}

static void init(void *u) {
  (void)u;
  make_palette();
  cw_debug_i32("look.tick", &tick);
  cw_debug_f32("look.weight", &weight);
  cw_debug_f32("look.lum_a", &lum_a);
  cw_debug_f32("look.lum_b", &lum_b);
  cw_debug_i32("look.lum_ok", &lum_ok);
  cw_debug_i32("look.split", &split_on);
  cw_debug_i32("look.array", &array_used);
  cw_debug_i32("look.pass_ran", &pass_ran);
  cw_debug_f32("look.gpu_ms", &gpu_ms);
  cw_debug_command("look", "look A [B [T]]: draw looks A and B at weight T (none ascii halfblock palette levels bayer bluenoise diffuse duotone custom)", cmd_look);
  cw_node world = cw_node_get(cw_tree_root(cw_scene_app_tree()), "World");
  cams[0] = cw_node_get(world, "Camera");
  cw_node3d_look_at(cams[0], (t3_vec3){ 0, 1.2f, -1 }, (t3_vec3){ 0, 1, 0 });
  cams[1] = cw_node_new("Camera3D", "Camera2");
  cw_node_add_child(world, cams[1]);
  cw_node_set(cams[1], "fov", cw_val_float(55));
  cw_node_set(cams[1], "near", cw_val_float(0.1f));
  cw_node_set(cams[1], "far", cw_val_float(300));
  cw_node3d_set_position(cams[1], (t3_vec3){ -9, 3.5f, 6 });
  cw_node3d_look_at(cams[1], (t3_vec3){ 0, 1.2f, -2 }, (t3_vec3){ 0, 1, 0 });
}

static void fixed_update(void *u, double dt) {
  (void)u;
  (void)dt;
  tick++;
  if (envi("LOOK_ORBIT", 0)) {
    float a = (float)tick * 0.01f;
    cw_node3d_set_position(cams[0], (t3_vec3){ sinf(a) * 11, 4.5f, cosf(a) * 11 });
    cw_node3d_look_at(cams[0], (t3_vec3){ 0, 1.2f, -1 }, (t3_vec3){ 0, 1, 0 });
  }
  apply();
}

static void render(void *u, double alpha) {
  (void)u;
  (void)alpha;
  cw_pipeline *p = cw_pipeline_of(cw_scene_app_tree());
  float l[2];
  lum_ok = cw_pipeline_look_luminance(p, l) ? 1 : 0;
  array_used = cw_pipeline_views_array_used(p, NULL) ? 1 : 0;
  if (lum_ok) lum_a = l[0], lum_b = l[1];
  const cw_pass_stat *st;
  int n = cw_pipeline_pass_stats(p, &st);
  pass_ran = 0;
  for (int i = 0; i < n; i++)
    if (!strcmp(st[i].name, "looks")) gpu_ms = st[i].gpu_ms_avg, pass_ran = st[i].ran;
  if (!envi("LOOK_BOXES", 0)) return;
  cw_r2d_begin(NULL);
  cw_vec2 cs = cw_r2d_canvas_size();
  float W = cs.x, H = cs.y;
  cw_r2d_text(cw_ui_font_sans(), "SCORE 012345", (cw_vec2){ W * 0.05f, H * 0.075f },
              &(cw_text_style){ .size = H * 0.05f, .color = { 1, 1, 1, 1 } });
  cw_r2d_text(cw_ui_font_sans(), "the plain scene here", (cw_vec2){ W * 0.64f, H * 0.83f },
              &(cw_text_style){ .size = H * 0.04f, .color = { 1, 0.9f, 0.4f, 1 } });
  cw_r2d_end();
}

cw_app_desc cw_main(int argc, char **argv) {
  (void)argc;
  (void)argv;
  static cw_app_desc base = { .title = "looks", .width = LOOKS_PERF ? 1920 : 960, .height = LOOKS_PERF ? 1080 : 540,
                              .render_scale = LOOKS_PERF ? 1 : 0, .debug = true, .init = init,
                              .fixed_update = fixed_update, .render = render };
  return cw_scene_app("main.cwscene", &base);
}

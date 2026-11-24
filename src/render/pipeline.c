/* The render pipeline: per-tree pass graph, target pool, GPU timing, and the
 * frame (pipeline.h). The built-in passes live in pipeline_post.c. */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>

#include "cartwheel/gpu.h"
#include "cartwheel/debug.h"
#include "cartwheel/profile.h"
#include "cartwheel/platform.h"
#include "cartwheel/render3d.h"
#include "pipeline_internal.h"
#include "view_layers.h"
#include "quality_internal.h"
#include "shadow_cull.h"
#include "pipeline_wgpu.h"
#include "scene/render3d_internal.h"
#include "lighting_internal.h"
#include "view_mask.h"

#ifdef T3_MISSING_HOOK
/* A draw whose program state no table has (three.c skips it): say which node, mesh and material it is, once per
 * state, so the log names what vanished and not only the state. The node is the nearest Node3D whose object is
 * the drawn object or one of its ancestors (a glTF instance's meshes sit under the node that holds it). */
struct missing_find { const t3_object *chain[32]; int n, best_depth; cw_node best; };
static bool missing_visit(cw_node n, void *user) {
  struct missing_find *f = user;
  t3_object *o = cw_node3d_object(n);
  if (!o) return true;
  for (int i = 0; i < f->n && i < f->best_depth; i++)
    if (f->chain[i] == o) { f->best = n, f->best_depth = i; break; }
  return true;
}
static void missing_hook(void *user, const char *state, const t3_object *o, const t3_material *m) {
  cw_tree *t = user;
  struct missing_find f = { .best_depth = 1 << 30 };
  for (const t3_object *p = o; p && f.n < 32; p = p->parent) f.chain[f.n++] = p;
  if (t && f.n) cw_node_walk(cw_tree_root(t), missing_visit, &f);
  char path[256] = "";
  if (f.best_depth < (1 << 30)) cw_node_path(f.best, (cw_node){ 0 }, path, sizeof path);
  static const char *const kinds[] = { [T3_MESH_BASIC_MATERIAL] = "MeshBasicMaterial", [T3_MESH_LAMBERT_MATERIAL] = "MeshLambertMaterial",
    [T3_MESH_PHONG_MATERIAL] = "MeshPhongMaterial", [T3_MESH_STANDARD_MATERIAL] = "MeshStandardMaterial",
    [T3_MESH_PHYSICAL_MATERIAL] = "MeshPhysicalMaterial", [T3_MESH_NORMAL_MATERIAL] = "MeshNormalMaterial",
    [T3_POINTS_MATERIAL] = "PointsMaterial", [T3_LINE_BASIC_MATERIAL] = "LineBasicMaterial", [T3_SPRITE_MATERIAL] = "SpriteMaterial" };
  const char *kind = m && (int)m->type >= 0 && (size_t)m->type < sizeof kinds / sizeof kinds[0] && kinds[m->type] ? kinds[m->type] : "material";
  cw_error("render: NOT DRAWN: %s%s%s%s%s%s, %s%s%s%s: its program state %s is in none of the app's program tables "
           "(the fix is in the next error)",
           path[0] ? "node \"" : "an object", path, path[0] ? "\"" : "", o && o->name && *o->name ? " (mesh \"" : "",
           o && o->name && *o->name ? o->name : "", o && o->name && *o->name ? "\")" : "", kind,
           m && m->name && *m->name ? " \"" : "", m && m->name && *m->name ? m->name : "", m && m->name && *m->name ? "\"" : "",
           state);
}
#endif

/* Values an extension hook set that the draw's program lacks (three.c's miss hook): an app program table generated
 * before the extension gained the value draws without it, silently. Each program state and name is warned about once,
 * listed in the debug fields cw.three.dropped and cw.three.dropped_values (the gates fail on any) and, with
 * CW_PROGRAMS_LOG_DIR (tools/programs.mjs), written to <dir>/<app>.dropped (the program survey fails on any). */
static char dropped_text[8192] = "\n"; /* "\n<state> <uniform|texture> <name>\n..." */
static int32_t dropped_count;
bool cw__ext_miss_quiet;
#ifdef T3_EXTENSION_MISS_HOOK
static void ext_miss_hook(void *user, const char *state, const char *name, bool texture) {
  (void)user;
  /* a hook setting what one variant of its part lacks on purpose (cw__ext_miss_quiet around those sets); and a caster's
   * depth program, which keeps only what shapes the geometry (a part's hook fills it as it fills the colour program;
   * a table out of date there is the build's stamp check's to catch) */
  if (cw__ext_miss_quiet || !strncmp(state, "depth", 5)) return;
  char line[400];
  snprintf(line, sizeof line, "\n%s %s %s\n", state, texture ? "texture" : "uniform", name);
  if (strstr(dropped_text, line)) return;
  size_t n = strlen(dropped_text), l = strlen(line);
  if (n + l - 1 < sizeof dropped_text) memcpy(dropped_text + n, line + 1, l); /* (the line's leading newline is the text's last) */
  dropped_count++;
  static bool field;
  if (!field) {
    field = true;
    cw_debug_i32("cw.three.dropped", &dropped_count);
    cw_debug_bytes("cw.three.dropped_values", dropped_text + 1, sizeof dropped_text - 1);
  }
  cw_error("render: the %s ext.%s set for program %s is DROPPED: the program has no such %s. Its program table was "
           "generated before the extension gained it; regenerate it: %s",
           texture ? "texture" : "uniform", name, state, texture ? "texture" : "uniform", cw__programs_fix_command());
#if !CW_CART
  const char *dir = getenv("CW_PROGRAMS_LOG_DIR");
  if (dir && *dir) {
    char path[1024];
    snprintf(path, sizeof path, "%s/%s.dropped", dir, cw__app_target_name());
    FILE *fp = fopen(path, "a");
    if (fp) fprintf(fp, "%s %s %s\n", state, texture ? "texture" : "uniform", name), fclose(fp);
  }
#endif
}
#endif

/* ── settings ───────────────────────────────────────────────────────── */

static void settings_default(cw_post_settings *s) {
  memset(s, 0, sizeof *s);
  s->hdr = true;
  s->exposure = 1;
  s->tonemap = CW_TONEMAP_AGX;
  s->bloom_intensity = 0.05f;
  s->bloom_threshold = 0;
  s->bloom_knee = 0.5f;
  s->bloom_radius = 1;
  s->bloom_levels = 6;
  s->ssao_radius = 0.5f;
  s->ssao_intensity = 1;
  s->ssao_power = 1.5f;
  s->ssao_slices = 2;
  s->ssao_steps = 6;
  s->ssr_max_distance = 1;
  s->ssr_thickness = 0.1f;
  s->ssr_quality = 0.5f;
  s->ssr_intensity = 1;
  s->ssgi_radius = 12;
  s->dof_focus_distance = 3;
  s->dof_focus_range = 1;
  s->dof_blur = 8;
  s->dof_near_blur = 0.5f;
  s->auto_exposure_key = 0.18f;
  s->auto_exposure_min_ev = -3;
  s->auto_exposure_max_ev = 3;
  s->auto_exposure_speed_up = 3;
  s->auto_exposure_speed_down = 1;
  s->motion_blur_shutter = 0.5f;
  s->motion_blur_samples = 10;
  s->motion_blur_max = 32;
  s->glint_threshold = 2;
  s->glint_intensity = 0.2f;
  s->glint_length = 0.12f;
  s->glint_points = 4;
  s->glint_rotation = 0.35f;
  s->ssgi_intensity = 10;
  s->ssgi_ao_intensity = 1;
  s->ssgi_thickness = 1;
  s->ssgi_slices = 2;
  s->ssgi_steps = 8;
  s->taa_feedback = 0.9f;
  s->taa_samples = 8;
  s->grade_intensity = 1;
  s->vignette_intensity = 0.35f;
  s->vignette_smoothness = 0.5f;
  s->grain_intensity = 0.04f;
  s->ca_intensity = 0.004f;
}

/* ── lifetime ───────────────────────────────────────────────────────── */

static void pass_free(pass_rec *r) {
  if (r->d.destroy) r->d.destroy(r->d.user);
  if (r->q_init) glDeleteQueries(CW__QUERY_RING, r->q);
  cw_free(r);
}

static void pipeline_free(void *ptr) {
  cw_pipeline *p = ptr;
  for (int i = 0; i < p->passes.count; i++) pass_free(p->passes.data[i]);
  cw_array_free(&p->passes);
  for (int i = 0; i < p->steps.count; i++) pass_free(p->steps.data[i]);
  cw_array_free(&p->steps);
  for (int i = 0; i < p->pool.count; i++) {
    cw__rt_destroy(&p->pool.data[i]->rt);
    cw_free(p->pool.data[i]);
  }
  cw_array_free(&p->pool);
  for (int i = 0; i < p->persist.count; i++) {
    cw__rt_destroy(&p->persist.data[i]->rt);
    cw_free(p->persist.data[i]);
  }
  cw_array_free(&p->persist);
  if (p->msaa_rt.fbo) cw__rt_destroy(&p->msaa_rt);
  cw__rt_destroy(&p->warm_m), cw__rt_destroy(&p->warm_c);
  if (p->arr_cam) t3_release(p->arr_cam);
  cw__pw_forget(p);
  if (p->msaa_fbo) {
    glDeleteFramebuffers(1, &p->msaa_fbo);
    glDeleteRenderbuffers(1, &p->msaa_color);
    glDeleteRenderbuffers(1, &p->msaa_depth);
  }
  cw_array_free(&p->stats);
  cw_free(p->missing_states);
  cw_free(p);
}

static int gpu_timing = -1; /* -1: CW_GPU_TIMERS not read yet */
bool cw__gpu_timing_wanted(void) {
  if (gpu_timing < 0) {
    const char *e = getenv("CW_GPU_TIMERS");
    gpu_timing = e && atoi(e) > 0;
  }
  return gpu_timing > 0;
}
void cw__gpu_timing_request(void) { gpu_timing = 1; }

/* debug channel: "pipeline" lists the passes, what ran, GPU ms, caps. The
 * first call starts the GPU timers (off until something reads them): pass
 * GPU times show from a later call on, up to 11 frames after it (GL reads them back in a batch every 8 frames). */
/* the feature a GL call on a WebGPU run is reported under (cartwheel/gl.h) */
#if CW_WGPU
#define GL_FEATURE(label) (cw__gl_feature = (label))
#else
#define GL_FEATURE(label) ((void)0)
#endif

/* pipeline_test gl_msaa_on_webgpu: the defect the dual-cart check
 * (tests/wgpu-carts.mjs) must catch, put back on purpose: a WebGPU run that
 * cannot multisample its scene asks GL for an MSAA framebuffer, as it did
 * before WebGPU fell back to single-sampled. It takes that path even where
 * the device could multisample the scene (a core WebGPU device, wasmcart
 * 0.37.0 on), so the control still runs the GL call there. Its control only. */
static bool test_gl_msaa_on_webgpu;
/* pipeline_test views_stall <ms>: a one-pass split screen spends that much CPU time a view before its scene (the wall time
 * gate's control: a one pass that is slower than the views drawn on their own must fail it) */
static double test_views_stall_ms;
static void views_stall(void) {
  if (test_views_stall_ms <= 0) return;
  double t0 = cw__render3d_now_ms();
  while (cw__render3d_now_ms() - t0 < test_views_stall_ms) {
  }
}
static void cmd_pipeline_test(int argc, const char **argv, cw_reply *r) {
  if (argc == 2 && !strcmp(argv[1], "gl_msaa_on_webgpu")) {
    test_gl_msaa_on_webgpu = true;
    cw_reply_raw(r, "{\"ok\":true}");
  } else if (argc == 3 && !strcmp(argv[1], "views_stall")) {
    test_views_stall_ms = atof(argv[2]);
    cw_reply_printf(r, "{\"views_stall\":%g}", test_views_stall_ms);
  } else cw_reply_error(r, "usage: pipeline_test gl_msaa_on_webgpu | views_stall <ms>");
}

static int32_t views_submits_dbg;
static void cmd_pipeline(int argc, const char **argv, cw_reply *r) {
  gpu_timing = 1;
  cw_tree *t = cw_current_tree();
  cw_pipeline *p = t ? cw_tree_slot(t, "pipeline") : NULL;
  if (!p) {
    cw_reply_error(r, "no pipeline on the current tree (it is made on the first 3D frame)");
    return;
  }
  /* pipeline views_array 0|1: split screen in one pass or view by view (cw_pipeline_set_views_array; A/B timing) */
  if (argc >= 3 && !strcmp(argv[1], "views_array")) cw_pipeline_set_views_array(p, atoi(argv[2]) != 0);
  /* pipeline views_submit 0|1: a one-pass split screen's views drawn and sent to the GPU one by one (WebGPU: submitted, GL: flushed), or all recorded first (A/B timing) */
  if (argc >= 3 && !strcmp(argv[1], "views_submit")) p->views_submit = atoi(argv[2]) != 0;
  /* pipeline shared_post 0|1: split screen, one post chain over the laid-out views or a chain of each view's own (A/B timing) */
  if (argc >= 3 && !strcmp(argv[1], "shared_post")) p->views_shared_post = atoi(argv[2]) != 0;
  static const char *const phases[] = { "shadow", "prepass", "opaque", "sky", "effects", "post", "overlay", "present" };
  cw_reply_printf(r, "{\"hdr_format\":\"%s\",\"timer_query\":%s,\"max_samples\":%d,\"gpu_ms\":%.4f,\"passes\":[",
                  cw__caps.hdr_format ? cw__caps.hdr_format : "?",
                  cw__caps.timer_query ? "true" : "false", cw__caps.max_samples, p->gpu_total_ms);
  for (int i = 0; i < p->stats.count; i++) {
    const cw_pass_stat *st = &p->stats.data[i];
    cw_reply_printf(r, "%s{\"name\":\"%s\",\"phase\":\"%s\",\"ran\":%s,\"fused\":%s,\"gpu_ms\":%.4f,\"cpu_ms\":%.4f}",
                    i ? "," : "", st->name, phases[st->phase], st->ran ? "true" : "false", st->fused ? "true" : "false",
                    st->gpu_ms_avg, st->cpu_ms_avg);
  }
  cw_reply_raw(r, "]");
  cw_reply_printf(r, ",\"views_array\":%s,\"views_array_why\":\"%s\"", p->arr_used ? "true" : "false",
                  !p->arr_used && p->arr_why ? p->arr_why : "");
  cw_reply_printf(r, ",\"view_hook\":%s,\"view_masks\":%d", p->view_hook ? "true" : "false", cw__view_masks_count());
  cw_reply_printf(r, ",\"post_prewarms\":%d,\"post_prewarm_armed\":%s,\"scene_prewarms\":%d", (int)p->post_prewarms,
                  p->post_warm_armed ? "true" : "false", (int)p->scene_prewarms);
  {
    /* the states three.c's generated table lacked (a JSON string, one per line) */
    cw_reply_raw(r, ",\"missing_states\":\"");
    for (const char *c = p->missing_states ? p->missing_states : ""; *c; c++) {
      if (*c == '\n') cw_reply_raw(r, "\\n");
      else if (*c == '"' || *c == '\\') cw_reply_printf(r, "\\%c", *c);
      else if ((unsigned char)*c >= 0x20) cw_reply_printf(r, "%c", *c);
    }
    cw_reply_raw(r, "\"");
    /* the values extension hooks set that their programs lack (ext_miss_hook), one per line */
    cw_reply_raw(r, ",\"dropped_values\":\"");
    for (const char *c = dropped_text + 1; *c; c++) {
      if (*c == '\n') cw_reply_raw(r, "\\n");
      else if (*c == '"' || *c == '\\') cw_reply_printf(r, "\\%c", *c);
      else if ((unsigned char)*c >= 0x20) cw_reply_printf(r, "%c", *c);
    }
    cw_reply_raw(r, "\"");
  }
  cw_reply_raw(r, "}");
}

cw_pipeline *cw_pipeline_of(cw_tree *t) {
  cw_pipeline *p = cw_tree_slot(t, "pipeline");
  if (p) return p;
  p = cw_calloc(1, sizeof *p, CW_MEM_RENDER);
  p->tree = t;
  settings_default(&p->s);
  p->views_array = true; /* split screen with a shared post chain: one pass when every per-view pass can */
  p->views_submit = true;
  cw_tree_set_slot(t, "pipeline", p, pipeline_free);
  cw__pipeline_add_builtin_passes(p);
  static bool cmd_done;
  if (!cmd_done) {
    cmd_done = true;
    cw_debug_i32("cw.views_submits", &views_submits_dbg); /* the last frame's one-pass views sent to the GPU one by one (WebGPU: submitted, GL: flushed; 0: none) */
    cw_debug_command("pipeline", "pipeline [views_array 0|1] [views_submit 0|1] [shared_post 0|1]: render passes (phase, ran, GPU ms), HDR format, timer support, missing three.c states; views_array sets split screen in one pass on or off first, views_submit its views sent to the GPU one by one or together, shared_post one post chain over the views or one a view", cmd_pipeline);
    cw_debug_command("pipeline_test", "pipeline_test gl_msaa_on_webgpu | views_stall <ms>: test controls: puts back a GL call on WebGPU runs that the dual-cart check must catch; a one-pass split screen slowed by ms a view",
                     cmd_pipeline_test);
  }
  return p;
}

cw_post_settings *cw_pipeline_settings(cw_pipeline *p) { return &p->s; }

void cw_post_prewarm(cw_pipeline *p, const cw_post_settings *later) {
  if (!p) return;
  p->post_warm_have = later != NULL;
  p->post_warm_armed = later != NULL;
  if (later) p->post_warm_s = *later;
  p->scene_warm_armed = true;
}
int cw_pipeline_post_prewarms(cw_pipeline *p) { return p ? p->post_prewarms : 0; }
void cw__post_prewarm_again(cw_pipeline *p, bool on) {
  if (!p) return;
  p->post_warm_armed = on && p->post_warm_have;
  p->scene_warm_armed = on;
}

/* ── the scene drawn once with everything in it (the prewarms) ──────── */

enum { SHOW_VISIBLE = 1, SHOW_CULLED = 2, SHOW_COUNT0 = 4 };
static void show_one(t3_object *o, void *ctx) {
  cw__show_keep *k = ctx;
  uint8_t f = (uint8_t)((o->visible ? SHOW_VISIBLE : 0) | (o->frustum_culled ? SHOW_CULLED : 0));
  if (o->type == T3_INSTANCED_MESH) {
    t3_instanced_mesh *im = (t3_instanced_mesh *)o;
    if (im->count == 0 && im->instance_matrix && im->instance_matrix->count >= 1) f |= SHOW_COUNT0, im->count = 1;
  }
  if (o->visible && !o->frustum_culled && !(f & SHOW_COUNT0)) return;
  if (k->n == k->cap) {
    k->cap = k->cap ? k->cap * 2 : 256;
    k->o = realloc(k->o, sizeof *k->o * (size_t)k->cap);
    k->flags = realloc(k->flags, (size_t)k->cap);
  }
  k->o[k->n] = o, k->flags[k->n++] = f;
  o->visible = true;
  o->frustum_culled = false;
}
/* (t3_object_traverse skips nothing: a hidden object's children are reached too) */
void cw__scene_show_all(t3_scene *sc, cw__show_keep *k) {
  memset(k, 0, sizeof *k);
  if (sc) t3_object_traverse(sc, show_one, k);
}
void cw__scene_show_restore(cw__show_keep *k) {
  for (int i = 0; i < k->n; i++) {
    t3_object *o = k->o[i];
    o->visible = (k->flags[i] & SHOW_VISIBLE) != 0;
    o->frustum_culled = (k->flags[i] & SHOW_CULLED) != 0;
    if (k->flags[i] & SHOW_COUNT0) ((t3_instanced_mesh *)o)->count = 0;
  }
  free(k->o), free(k->flags);
  memset(k, 0, sizeof *k);
}

void cw__scene_warm_draw(cw_pass_ctx *ctx, t3_texture *env, bool gi) {
  cw_pipeline *p = ctx->pipeline;
  t3_renderer *r = ctx->renderer;
  t3_scene *sc = ctx->scene3d;
  const cw_rt *scene = p->scene_rt, *draw = p->scene_draw_rt;
  bool wg = cw_gpu_backend() == CW_GPU_WEBGPU;
  /* (GL: a program is the same whatever it draws into, so the scene drawn straight to the window has one too) */
  if (!r || !sc || !ctx->camera || (wg && (!scene || !draw))) return;
  CW_PROFILE_SCOPE("pipeline.scene_warm");
  bool ms = scene && draw && draw != scene;
  cw_rt_format fmt = scene ? scene->format : CW_RT_RGBA8;
  enum { WS = 16 }; /* (a program or a pipeline is the same at any size) */
  if (!p->warm_c.fbo || p->warm_c.format != fmt) {
    cw__rt_destroy(&p->warm_m), cw__rt_destroy(&p->warm_c);
    if (!cw__rt_create(&p->warm_c, WS, WS, fmt, true)) return;
  }
  if (wg && ms && !p->warm_m.fbo && !cw__pw_msaa_create(&p->warm_m, &p->warm_c)) return;
  t3_texture *own = sc->environment;
  if (env) {
    if (own) t3_retain(own);
    t3_scene_set_environment(sc, env);
  }
  int n0 = gi ? cw__gi_tag_scene(ctx->tree, r, sc) : -1;
  /* every object shown only for an app that said what it turns on later (cw_post_prewarm): an object hidden for good
   * would otherwise be drawn in a state no program table was surveyed with (a missing state reported) */
  cw__show_keep keep = { 0 };
  if (p->post_warm_have) cw__scene_show_all(sc, &keep);
  t3_renderer_set_shadow_hold(r, true);
  if (wg) {
    cw__pw_bind_target(ms ? &p->warm_m : &p->warm_c);
    const cw__wgpu_pass_targets *pt = cw__pass_wgpu_targets(ctx);
    if (pt && pt->color) {
      t3_external_target et = {
        .color = (uintptr_t)pt->color, .color_resolve = (uintptr_t)pt->resolve, .depth = (uintptr_t)pt->depth,
        .color_format = (int)pt->color_format, .depth_format = (int)pt->depth_format, .samples = pt->samples,
        .load_color = false, .load_depth = false, .clear_rgba = { 0, 0, 0, 1 }, .clear_depth = cw_gpu_reversed_depth() ? 0.0f : 1.0f,
        .x = 0, .y = 0, .w = pt->w, .h = pt->h, .flip_y = pt->flip_y,
      };
      t3_renderer_set_external_encoder(r, (uintptr_t)pt->enc);
      t3_renderer_set_external_target(r, &et);
      t3_renderer_render(r, sc, ctx->camera);
      t3_renderer_set_external_target(r, NULL);
    }
  } else {
    int bw, bh, vx, vy, vw, vh, sx, sy, sw, sh;
    t3_renderer_get_size(r, &bw, &bh);
    t3_renderer_get_viewport(r, &vx, &vy, &vw, &vh);
    bool st = t3_renderer_get_scissor(r, &sx, &sy, &sw, &sh);
    bool auto_clear = t3_renderer_get_auto_clear(r);
    t3_renderer_reset_bindings(r);
    glBindFramebuffer(GL_FRAMEBUFFER, p->warm_c.fbo);
    t3_renderer_set_framebuffer(r, p->warm_c.fbo);
    t3_renderer_set_viewport(r, 0, bh - WS, WS, WS);
    t3_renderer_set_scissor(r, 0, bh - WS, WS, WS);
    t3_renderer_set_scissor_test(r, true);
    t3_renderer_set_auto_clear(r, true);
    t3_renderer_render(r, sc, ctx->camera);
    t3_renderer_set_auto_clear(r, auto_clear);
    t3_renderer_set_viewport(r, vx, vy, vw, vh);
    t3_renderer_set_scissor(r, sx, sy, sw, sh);
    t3_renderer_set_scissor_test(r, st);
    t3_renderer_set_framebuffer(r, p->bound_fbo);
    t3_renderer_reset_bindings(r);
    t3_renderer_reset_state(r);
    glBindFramebuffer(GL_FRAMEBUFFER, p->bound_fbo);
  }
  t3_renderer_set_shadow_hold(r, false);
  cw__scene_show_restore(&keep);
  if (n0 >= 0) cw__gi_untag_from(ctx->tree, n0);
  if (env) {
    t3_scene_set_environment(sc, own);
    if (own) t3_release(own);
  }
  if (!cw__render3d_shadows_kept(ctx->tree)) t3_renderer_shadow_needs_update(r);
}

void cw_pipeline_exposure_reset(cw_pipeline *p) {
  if (p) p->auto_exposure_reset = true;
}

const char *cw_pipeline_missing_states(cw_tree *t) {
  cw_pipeline *p = t ? cw_tree_slot(t, "pipeline") : NULL;
  return p && p->missing_states ? p->missing_states : "";
}

void cw_pipeline_set_output(cw_pipeline *p, const cw_rt *rt) { p->output_user = rt; }

void cw_pipeline_set_app_render(cw_pipeline *p, void (*render)(void *user, double alpha), void *user) {
  p->app_render = render;
  p->app_user = user;
}

/* ── passes ─────────────────────────────────────────────────────────── */

static pass_rec *find_pass(cw_pipeline *p, const char *name) {
  const char *n = cw_intern(name);
  for (int i = 0; i < p->passes.count; i++)
    if (p->passes.data[i]->name == n) return p->passes.data[i];
  return NULL;
}

/* the index of an active pass (-1: none or inactive) */
static int active_index(cw_pipeline *p, const char *name) {
  const char *n = cw_intern(name);
  for (int i = 0; i < p->passes.count; i++)
    if (p->passes.data[i]->name == n) return p->passes.data[i]->active ? i : -1;
  return -1;
}
/* passes a and b (indices, a < b) are neighbours in the POST chain: no active
 * colour-writing POST pass runs between them */
static bool post_adjacent(cw_pipeline *p, int a, int b) {
  if (a < 0 || b < 0 || a >= b) return false;
  for (int i = a + 1; i < b; i++) {
    pass_rec *r = p->passes.data[i];
    if (r->active && r->d.phase == CW_PHASE_POST && (r->d.writes & CW_RES_COLOR)) return false;
  }
  return true;
}
/* a SKY / EFFECTS pass a and a POST pass b are neighbours: nothing active
 * between them writes the colour */
static bool fx_post_adjacent(cw_pipeline *p, int a, int b) {
  if (a < 0 || b < 0 || a >= b) return false;
  for (int i = a + 1; i < b; i++) {
    pass_rec *r = p->passes.data[i];
    if (r->active && r->d.phase >= CW_PHASE_SKY && r->d.phase <= CW_PHASE_POST && (r->d.writes & CW_RES_COLOR)) return false;
  }
  return true;
}
/* the same with the pass `but` (an index, -1 none) left out of the check */
static bool fx_post_adjacent_but(cw_pipeline *p, int a, int b, int but) {
  if (a < 0 || b < 0 || a >= b) return false;
  for (int i = a + 1; i < b; i++) {
    pass_rec *r = p->passes.data[i];
    if (i != but && r->active && r->d.phase >= CW_PHASE_SKY && r->d.phase <= CW_PHASE_POST && (r->d.writes & CW_RES_COLOR)) return false;
  }
  return true;
}
/* post fusion (pipeline_internal.h): only the built-in passes, next to each
 * other in the chain, with nothing that needs the image in between */
static bool test_no_fusion(void) { /* CW_TEST_NO_FUSION=1 (native): the passes run separately (A/B, gate controls) */
#if CW_CART
  return false;
#else
  static int v = -1;
  if (v < 0) v = getenv("CW_TEST_NO_FUSION") && atoi(getenv("CW_TEST_NO_FUSION")) ? 1 : 0;
  return v == 1;
#endif
}
bool cw__fusion_raw(void) {
#if CW_CART
  return false;
#else
  static int v = -1;
  if (v < 0) v = getenv("CW_TEST_FUSION_RAW") && atoi(getenv("CW_TEST_FUSION_RAW")) ? 1 : 0;
  return v == 1;
#endif
}
/* a fused program's name is its pass's program and its steps joined by '+'
 * ("tonemap+bloom+grade", "ssao.composite+fog"): does it run `step` (a
 * program name, "bloom.composite" for bloom's composite)? */
bool cw__program_has_step(const char *program, const char *step) {
  if (!strcmp(program, step)) return true;
  const char *plus = strchr(program, '+');
  if (!plus) return false;
  if ((size_t)(plus - program) == strlen(step) && !strncmp(program, step, (size_t)(plus - program))) return true;
  if (!strcmp(step, "bloom.composite")) step = "bloom";
  size_t n = strlen(step);
  for (const char *t = plus; t; t = strchr(t + 1, '+'))
    if (!strncmp(t + 1, step, n) && (t[1 + n] == '+' || t[1 + n] == 0)) return true;
  return false;
}

/* a pass that runs inside another this frame: reported as ran and fused */
static void fuse_off(cw_pipeline *p, int i) {
  pass_rec *r = p->passes.data[i];
  r->active = false;
  r->stat.ran = r->stat.fused = true;
}
static void plan_fusion(cw_pipeline *p, const cw_post_settings *s, bool post, bool own_view) {
  p->fuse_fog = p->fuse_bloom = p->fuse_grade = p->fuse_lens = p->fuse_lens_tm = p->fuse_distort = p->fuse_aa_tm = p->fuse_fog_tm = false;
  p->bloom_glow = NULL;
  if (!post || s->debug_view != CW_VIEW_NONE || test_no_fusion()) return;
  int bl = active_index(p, "bloom"), tm = active_index(p, "tonemap"), gr = active_index(p, "grade");
  int aa = active_index(p, "aa"), ln = active_index(p, "lens");
  int fg = active_index(p, "height_fog"), ao = active_index(p, "ssao");
  /* the fog over the scene, then the SSAO composite reading it: one pass.
   * Not under TAA: its history and clipping carry a rounding step at one
   * pixel on into many frames, where it grows past a step */
  if (fg >= 0 && ao >= 0 && !s->taa && fx_post_adjacent(p, fg, ao)) {
    p->fuse_fog = true;
    fuse_off(p, fg);
  }
  if (tm >= 0 && post_adjacent(p, bl, tm)) p->fuse_bloom = true;
  /* the distortion read inside the tonemap pass (opt in): nothing but a fused bloom between them */
  int ds = active_index(p, "distort");
  if (s->fuse_distortion && ds >= 0 && tm > ds) {
    bool ok = true;
    for (int i = ds + 1; i < tm && ok; i++) {
      pass_rec *r = p->passes.data[i];
      if (r->active && r->d.phase == CW_PHASE_POST && (r->d.writes & CW_RES_COLOR) && !(i == bl && p->fuse_bloom)) ok = false;
    }
    if (ok) {
      p->fuse_distort = true;
      fuse_off(p, ds);
    }
  }
  if (tm >= 0 && post_adjacent(p, tm, gr)) {
    p->fuse_grade = true;
    fuse_off(p, gr);
  }
  /* effects detail Low and Very Low (cartwheel/quality.h): the height fog read inside the tonemap pass (nothing between
   * them draws colour but a bloom whose composite is fused there too), one full-frame blend fewer. With bloom (Low) the
   * glow is gathered from the scene before the fog, the composite adds it after */
  /* (only in a run that drew its own view: split screen's shared post has no one camera or depth for it) */
  if (own_view && !p->fuse_fog && fg >= 0 && tm >= 0 && !p->fuse_distort && cw__quality_fused_post() &&
      fx_post_adjacent_but(p, fg, tm, p->fuse_bloom ? bl : -1)) {
    p->fuse_fog_tm = true;
    fuse_off(p, fg);
  }
  /* effects detail Low and Very Low (cartwheel/quality.h): FXAA inside the tonemap pass, its edge search on the HDR
   * scene (a display luma approximated from each read), one full-frame pass fewer. With bloom's composite fused in
   * (Low) each read FXAA blends takes the glow at the pixel (the glow is smooth over FXAA's few pixels). Not with the
   * distortion fused in (FXAA reads its neighbours, which it would have to be worked out for too) */
  if (tm >= 0 && aa >= 0 && s->aa == CW_AA_FXAA && !p->fuse_distort && cw__quality_fused_post() &&
      post_adjacent(p, tm, aa)) {
    p->fuse_aa_tm = true;
    fuse_off(p, aa);
    aa = -1;
  }
  /* the lens reads only its own pixel unless chromatic aberration is on */
  /* (and with a lens per view of a shared split screen chain: the lens pass finds each pixel's view) */
  if (ln >= 0 && !s->chromatic_aberration && !(s->vignette_per_view && p->nviews >= 2 && p->view_post_split)) {
    if (aa >= 0 && post_adjacent(p, aa, ln)) p->fuse_lens = true;
    else if (aa < 0 && tm >= 0 && (gr < 0 || p->fuse_grade) && post_adjacent(p, tm, ln)) p->fuse_lens_tm = true;
    if (p->fuse_lens || p->fuse_lens_tm) fuse_off(p, ln);
  }
}

bool cw_pipeline_add_pass(cw_pipeline *p, const cw_pass_desc *d) {
  CW_ASSERT(d && d->name && d->execute, "a pass needs a name and an execute function");
  CW_ASSERT(d->phase >= 0 && d->phase < CW_PHASE_COUNT, "phase out of range");
  if (find_pass(p, d->name)) {
    cw_error("cw_pipeline_add_pass: a pass named '%s' exists; remove it first or pick another name", d->name);
    return false;
  }
  pass_rec *r = cw_calloc(1, sizeof *r, CW_MEM_RENDER);
  r->d = *d;
  r->name = cw_intern(d->name);
  r->d.name = r->name;
  r->seq = p->next_seq++;
  r->enabled = !d->disabled;
  r->stat.name = r->name;
  r->stat.phase = d->phase;
  r->stat.gpu_ms = -1;
  r->stat.gpu_ms_avg = -1;
  r->stat.cpu_ms_avg = -1;
  cw_array_push(&p->passes, r);
  p->order_dirty = true;
  return true;
}

bool cw_pipeline_remove_pass(cw_pipeline *p, const char *name) {
  pass_rec *r = find_pass(p, name);
  if (!r) return false;
  for (int i = 0; i < p->passes.count; i++)
    if (p->passes.data[i] == r) {
      memmove(p->passes.data + i, p->passes.data + i + 1, (size_t)(p->passes.count - i - 1) * sizeof r);
      p->passes.count--;
      break;
    }
  pass_free(r);
  return true;
}

bool cw_pipeline_set_pass_enabled(cw_pipeline *p, const char *name, bool on) {
  pass_rec *r = find_pass(p, name);
  if (!r) {
    cw_warn("cw_pipeline_set_pass_enabled: no pass '%s'", name);
    return false;
  }
  r->enabled = on;
  return true;
}

bool cw_pipeline_has_pass(cw_pipeline *p, const char *name) { return find_pass(p, name) != NULL; }

static int pass_cmp(const void *a, const void *b) {
  const pass_rec *x = *(pass_rec *const *)a, *y = *(pass_rec *const *)b;
  if (x->d.phase != y->d.phase) return x->d.phase < y->d.phase ? -1 : 1;
  if (x->d.order != y->d.order) return x->d.order < y->d.order ? -1 : 1;
  return x->seq < y->seq ? -1 : x->seq > y->seq;
}

/* ── target pool ────────────────────────────────────────────────────── */

const cw_rt *cw_pass_rt(cw_pass_ctx *ctx, int w, int h, cw_rt_format f, bool with_depth) {
  cw_pipeline *p = ctx->pipeline;
  w = CW_MAX(w, 1);
  h = CW_MAX(h, 1);
  for (int i = 0; i < p->pool.count; i++) {
    pool_rt *e = p->pool.data[i];
    if (!e->used && e->rt.w == w && e->rt.h == h && e->rt.format == f && e->depth == with_depth) {
      e->used = true;
      e->last_frame = p->frame;
      return &e->rt;
    }
  }
  pool_rt *e = cw_calloc(1, sizeof *e, CW_MEM_RENDER);
  if (!cw__rt_create(&e->rt, w, h, f, with_depth)) {
    cw_error("render pipeline: a %dx%d target of format %d is not renderable on this host", w, h, (int)f);
    cw_free(e);
    return NULL;
  }
  e->depth = with_depth;
  e->used = true;
  e->last_frame = p->frame;
  cw_array_push(&p->pool, e);
  return &e->rt;
}

void cw_pass_rt_release(cw_pass_ctx *ctx, const cw_rt *rt) {
  cw_pipeline *p = ctx->pipeline;
  for (int i = 0; i < p->pool.count; i++)
    if (&p->pool.data[i]->rt == rt) p->pool.data[i]->used = false;
}

/* an extra view keeps targets of its own under each name: its size and history never disturb the frame's */
static const char *scoped_key(const cw_pipeline *p, const char *key) {
  if (!p->rt_scope) return cw_intern(key);
  char k[96];
  snprintf(k, sizeof k, "%s@%s", key, p->rt_scope);
  return cw_intern(k);
}

const cw_rt *cw_pipeline_rt_persistent(cw_pipeline *p, const char *key, int w, int h, cw_rt_format f, bool *fresh) {
  const char *k = scoped_key(p, key);
  persist_rt *e = NULL;
  for (int i = 0; i < p->persist.count; i++)
    if (p->persist.data[i]->key == k) e = p->persist.data[i];
  if (!e) {
    e = cw_calloc(1, sizeof *e, CW_MEM_RENDER);
    e->key = k;
    cw_array_push(&p->persist, e);
  }
  if (fresh) *fresh = false;
  if (!e->rt.fbo || e->rt.w != w || e->rt.h != h || e->rt.format != f) {
    cw__rt_destroy(&e->rt);
    if (!cw__rt_create(&e->rt, w, h, f, false)) {
      cw_error("render pipeline: persistent target '%s' (%dx%d, format %d) is not renderable", key, w, h, (int)f);
      return NULL;
    }
    if (fresh) *fresh = true;
  }
  return &e->rt;
}

const cw_rt *cw__pipeline_rt_find(cw_pipeline *p, const char *key) {
  const char *k = scoped_key(p, key);
  for (int i = 0; i < p->persist.count; i++)
    if (p->persist.data[i]->key == k) return p->persist.data[i]->rt.fbo ? &p->persist.data[i]->rt : NULL;
  return NULL;
}

static const cw_rt *persistent_depth(cw_pipeline *p, const char *key, int w, int h, cw_rt_format f) {
  const char *k = scoped_key(p, key);
  persist_rt *e = NULL;
  for (int i = 0; i < p->persist.count; i++)
    if (p->persist.data[i]->key == k) e = p->persist.data[i];
  if (!e) {
    e = cw_calloc(1, sizeof *e, CW_MEM_RENDER);
    e->key = k;
    e->depth = true;
    cw_array_push(&p->persist, e);
  }
  if (!e->rt.fbo || e->rt.w != w || e->rt.h != h || e->rt.format != f) {
    cw__rt_destroy(&e->rt);
    if (!cw__rt_create(&e->rt, w, h, f, true)) {
      cw_error("render pipeline: the scene target (%dx%d, format %d) is not renderable", w, h, (int)f);
      return NULL;
    }
  }
  return &e->rt;
}

/* the HDR scene's format: RGBA16F, or on effects detail Low and Very Low R11G11B10 float where the device renders it
 * (half the bytes a pixel; no alpha, which nothing after the scene reads there: no SSAO, TAA, SSR or distortion).
 * CW_TEST_SMALL_HDR=0 (native) keeps RGBA16F (measuring what it saves) */
static cw_rt_format hdr_format(void) {
#if !CW_CART
  static int off = -1;
  if (off < 0) off = getenv("CW_TEST_SMALL_HDR") && !atoi(getenv("CW_TEST_SMALL_HDR"));
  if (off) return CW_RT_RGBA16F;
#endif
  return cw__small_hdr_render && cw__quality_small_hdr() ? CW__RT_RG11B10F : CW_RT_RGBA16F;
}

/* the scene's multisampled colour + depth, resolved into the scene target */
static unsigned msaa_fbo(cw_pipeline *p, int w, int h, cw_rt_format f, int samples) {
  samples = CW_MIN(samples, cw__caps.max_samples);
  if (samples < 2) return 0;
  if (p->msaa_failed == (int)f + 1) return 0; /* tried and refused by this host: logged once */
  if (p->msaa_fbo && p->msaa_w == w && p->msaa_h == h && p->msaa_format == f && p->msaa_samples == samples)
    return p->msaa_fbo;
  if (!p->msaa_fbo) {
    glGenFramebuffers(1, &p->msaa_fbo);
    glGenRenderbuffers(1, &p->msaa_color);
    glGenRenderbuffers(1, &p->msaa_depth);
  }
  glBindRenderbuffer(GL_RENDERBUFFER, p->msaa_color);
  glRenderbufferStorageMultisample(GL_RENDERBUFFER, samples, f == CW_RT_RGBA16F ? GL_RGBA16F : f == CW__RT_RG11B10F ? GL_R11F_G11F_B10F : GL_RGBA8, w, h);
  glBindRenderbuffer(GL_RENDERBUFFER, p->msaa_depth);
  glRenderbufferStorageMultisample(GL_RENDERBUFFER, samples, GL_DEPTH_COMPONENT24, w, h);
  glBindRenderbuffer(GL_RENDERBUFFER, 0);
  glBindFramebuffer(GL_FRAMEBUFFER, p->msaa_fbo);
  glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_RENDERBUFFER, p->msaa_color);
  glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_RENDERBUFFER, p->msaa_depth);
  if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) {
    cw_error("render pipeline: %dx MSAA on a %s target is not supported here; the scene renders without MSAA",
             samples, f == CW_RT_RGBA16F ? "RGBA16F" : f == CW__RT_RG11B10F ? "R11G11B10F" : "RGBA8");
    glDeleteFramebuffers(1, &p->msaa_fbo);
    glDeleteRenderbuffers(1, &p->msaa_color);
    glDeleteRenderbuffers(1, &p->msaa_depth);
    p->msaa_fbo = 0;
    p->msaa_failed = (int)f + 1;
    return 0;
  }
  p->msaa_w = w;
  p->msaa_h = h;
  p->msaa_format = f;
  p->msaa_samples = samples;
  return p->msaa_fbo;
}

/* ── GPU timing ─────────────────────────────────────────────────────── */

/* a frame's GPU time for a pass is the sum of its runs that frame (a pass
 * runs once per splitscreen view): the sum is published when a later
 * frame's result arrives */
static void gpu_time_add(pass_rec *r, uint64_t frame, float ms) {
  if (r->acc_on && frame < r->acc_frame) return; /* a frame already published (out of order) */
  if (r->acc_on && frame != r->acc_frame) {
    bool whole = true; /* every run of that frame was timed */
    for (int k = 0; k < 8; k++) whole &= r->missed[k] != r->acc_frame + 1;
    if (whole) {
      r->stat.gpu_ms = r->acc_ms;
      r->stat.gpu_ms_avg = r->stat.gpu_ms_avg < 0 ? r->acc_ms : r->stat.gpu_ms_avg + (r->acc_ms - r->stat.gpu_ms_avg) / 16.0f;
    }
    r->acc_ms = 0;
  }
  r->acc_on = true;
  r->acc_frame = frame;
  r->acc_ms += ms;
}

/* Reading the timers back is the expensive part, not the queries. Any GL
 * read waits for the driver: with Mesa's glthread (on by default) the first
 * one waits until the driver thread has run every command queued so far,
 * which serializes the game's thread with the driver's (6 ms a frame on the
 * 890M in the showcase when it happened every frame), and each availability
 * ask costs a round trip after that. So the results are read in a batch
 * every TIMER_EVERY frames, only those at least TIMER_LAG frames old (the GPU
 * is done with them), oldest first, and the first one not available ends a
 * pass's poll. The ring holds (TIMER_EVERY + TIMER_LAG) frames of up to four
 * views (CW__QUERY_RING). */
#define TIMER_LAG 3
#define TIMER_EVERY 8
static void timer_poll(pass_rec *r, bool disjoint, uint64_t frame) {
  for (;;) {
    int best = -1;
    for (int i = 0; i < CW__QUERY_RING; i++)
      if (r->q_busy[i] && (best < 0 || r->q_frame[i] < r->q_frame[best])) best = i;
    if (best < 0 || r->q_frame[best] + TIMER_LAG > frame) return;
    GLuint avail = 0;
    glGetQueryObjectuiv(r->q[best], GL_QUERY_RESULT_AVAILABLE, &avail);
    if (!avail) return; /* the GPU is further behind: next frame */
    GLuint ns = 0;
    glGetQueryObjectuiv(r->q[best], GL_QUERY_RESULT, &ns);
    r->q_busy[best] = false;
    if (!disjoint) {
      gpu_time_add(r, r->q_frame[best], (float)ns / 1e6f);
      if (CW_PROFILE_ON()) cw__profile_gpu(r->q_pname[best] ? r->q_pname[best] : r->name, r->q_eframe[best], (float)ns / 1e6f);
    }
  }
}

/* GL allows one active query per target, across every pipeline: a pipeline
 * rendered from inside another one's pass (an app pass drawing a second
 * tree into a target) must not begin a GL_TIME_ELAPSED query while that
 * pass's is open. GL rejects the nested begin, the nested end then closes the
 * OUTER query, the outer end fails, and the nested slot reads back a query
 * that never began: four invalid calls a frame, and a query sequence that
 * depends on GPU timing. Nested passes go untimed; the enclosing pass's time
 * includes them. */
static bool timer_open;

static int timer_begin(pass_rec *r, uint64_t frame, const char *pname) {
  if (!cw__caps.timer_query || timer_open || cw_gpu_backend() == CW_GPU_WEBGPU || !cw__gpu_timing_wanted()) return -1;
  if (!r->q_init) {
    glGenQueries(CW__QUERY_RING, r->q);
    r->q_init = true;
  }
  for (int i = 0; i < CW__QUERY_RING; i++)
    if (!r->q_busy[i]) {
      glBeginQuery(GL_TIME_ELAPSED_EXT, r->q[i]);
      r->q_busy[i] = true;
      r->q_frame[i] = frame;
      r->q_eframe[i] = cw_frame_index();
      r->q_pname[i] = pname;
      timer_open = true;
      return i;
    }
  r->missed[frame % 8] = frame + 1;
  return -1; /* every query still in flight: skip timing this frame */
}

static void timer_end(int slot) {
  if (slot < 0) return;
  glEndQuery(GL_TIME_ELAPSED_EXT);
  timer_open = false;
}

int cw_pipeline_pass_stats(cw_pipeline *p, const cw_pass_stat **out) {
  *out = p->stats.data;
  return p->stats.count;
}

float cw_pipeline_gpu_ms(cw_pipeline *p) { return p->gpu_total_ms; }

/* WebGPU timestamps of a pass arrived (pipeline_wgpu.c): one frame's, every
 * run of the pass that frame summed */
void cw__pipeline_gpu_time(cw_pipeline *p, const char *name, float ms) {
  pass_rec *r = find_pass(p, name);
  if (!r) return;
  r->stat.gpu_ms = ms;
  r->stat.gpu_ms_avg = r->stat.gpu_ms_avg < 0 ? ms : r->stat.gpu_ms_avg + (ms - r->stat.gpu_ms_avg) / 16.0f;
}

/* the frame's stats, once its last run is done (passes read the previous
 * frame's during a frame) */
static void frame_stats(cw_pipeline *p) {
  static bool field;
  static int32_t resolves;
  if (!field) {
    field = true;
    cw_debug_i32("cw.msaa_resolves", &resolves); /* WebGPU: the multisampled scene colour's resolves last frame, every view */
  }
  p->msaa_resolves_frame = resolves = p->msaa_resolves;
  cw_array_clear(&p->stats);
  p->gpu_total_ms = 0;
  for (int i = 0; i < p->passes.count; i++) {
    pass_rec *r = p->passes.data[i];
    if (r->stat.ran && !r->stat.fused) {
      float ms = r->frame_cpu;
      r->stat.cpu_ms = ms;
      r->stat.cpu_ms_avg = r->stat.cpu_ms_avg < 0 ? ms : r->stat.cpu_ms_avg + (ms - r->stat.cpu_ms_avg) / 16.0f;
    }
    cw_array_push(&p->stats, r->stat);
    if (r->stat.ran && !r->stat.fused && r->stat.gpu_ms > 0) p->gpu_total_ms += r->stat.gpu_ms;
  }
}

/* ── the frame ──────────────────────────────────────────────────────── */

static void mat4_from_camera(cw_pass_ctx *c) {
  t3_camera *cam = c->camera;
  t3_object_update_world_matrix(cam, true, false);
  t3_mat4_invert(&c->view, &cam->base.matrix_world);
  c->proj = cam->projection_matrix;
  t3_mat4_multiply(&c->view_proj, &c->proj, &c->view);
  t3_mat4_invert(&c->inv_proj, &c->proj);
  t3_mat4_invert(&c->inv_view_proj, &c->view_proj);
}

void cw__foliage_frame(bool msaa, bool taa, uint64_t frame); /* src/render/foliage.c */
static float halton(int i, int b) {
  float f = 1, r = 0;
  while (i > 0) {
    f /= (float)b;
    r += f * (float)(i % b);
    i /= b;
  }
  return r;
}

/* the profiler's GPU lane names a split-screen view's run of a pass apart ("ssao@v1") */
static const char *gpu_lane_name(const cw_pipeline *p, const pass_rec *r) {
  static const char *const SUF[CW_MAX_VIEWS] = { "@v0", "@v1", "@v2", "@v3" };
  if (p->nviews < 2 || p->view_i < 0 || p->view_i >= CW_MAX_VIEWS) return r->name;
  return cw__profile_name2(r->name, SUF[p->view_i]);
}

/* GPU work the pipeline itself does between passes (the views laid out, the MSAA resolve, the depth copies) is
 * timed under a step's name ("views", "resolve") in the profiler's GPU lane; steps are not passes (never listed,
 * never run by name) */
static pass_rec *step_rec(cw_pipeline *p, const char *name) {
  const char *n = cw_intern(name);
  for (int i = 0; i < p->steps.count; i++)
    if (p->steps.data[i]->name == n) return p->steps.data[i];
  pass_rec *r = cw_calloc(1, sizeof *r, CW_MEM_RENDER);
  r->name = n;
  r->d.name = n;
  r->stat.name = n;
  cw_array_push(&p->steps, r);
  return r;
}
static int step_begin(cw_pipeline *p, const char *name) {
  if (!CW_PROFILE_ON() || !cw__gpu_timing_wanted()) return -2;
  pass_rec *r = step_rec(p, name);
  const char *lane = gpu_lane_name(p, r);
  if (cw_gpu_backend() == CW_GPU_WEBGPU) {
    cw__pw_timing_pass(p, r->name, lane);
    return -1;
  }
  return timer_begin(r, p->frame, lane);
}
static void step_end(int slot) {
  if (slot == -2) return;
  if (cw_gpu_backend() == CW_GPU_WEBGPU) cw__pw_timing_pass(NULL, NULL, NULL);
  else timer_end(slot);
}

static void run_pass(cw_pipeline *p, pass_rec *r, cw_pass_ctx *ctx) {
  bool wgpu = cw_gpu_backend() == CW_GPU_WEBGPU;
  cw__pass_state_reset();
  if (CW_PROFILE_ON()) cw_profile_begin(cw__profile_name2("pass ", r->name));
  const char *lane = CW_PROFILE_ON() ? gpu_lane_name(p, r) : r->name;
  int slot = timer_begin(r, p->frame, lane);
  if (wgpu) cw__pw_timing_pass(p, r->name, lane);
  if (wgpu && CW_PROFILE_ON()) cw__pw_timing_whole(false); /* the profiler: three.c's renders for the pass too */
  ctx->user = r->d.user;
  double t0 = cw__render3d_now_ms();
  GL_FEATURE(r->name);
  r->d.execute(ctx, r->d.user);
  GL_FEATURE(NULL);
  r->frame_cpu += (float)(cw__render3d_now_ms() - t0);
  if (wgpu && CW_PROFILE_ON()) cw__pw_timing_whole(true);
  if (wgpu) cw__pw_timing_pass(p, NULL, NULL);
  timer_end(slot);
  r->stat.ran = true;
  CW_PROFILE_END();
}

/* bind the target a phase draws into */
static void bind_phase_target(cw_pass_ctx *ctx, const cw_rt *rt) {
  cw__vclip.bound_w = rt->w;
  cw__vclip.bound_h = rt->h;
  ctx->pipeline->bound_fbo = rt->fbo;
  ctx->pipeline->bound_depth_tex = rt->depth_tex;
  ctx->pipeline->bound_depth_rb = rt->fbo && rt->fbo == ctx->pipeline->msaa_fbo ? ctx->pipeline->msaa_depth : 0;
  if (cw_gpu_backend() == CW_GPU_WEBGPU) {
    cw__pw_bind_target(rt);
    return;
  }
  glBindFramebuffer(GL_FRAMEBUFFER, rt->fbo);
  glViewport(0, 0, rt->w, rt->h);
}

/* a gate control (native test runs): CW_TEST_SKIP_PASS=name never runs that pass */
static bool test_skipped(const char *name) {
#if CW_CART
  (void)name;
  return false;
#else
  static const char *skip;
  static bool read;
  if (!read) {
    read = true;
    skip = getenv("CW_TEST_SKIP_PASS");
    if (skip && *skip) cw_warn("CW_TEST_SKIP_PASS: pass '%s' never runs (a gate control)", skip);
  }
  if (!skip) return false;
  size_t n = strlen(name); /* a comma separated list */
  for (const char *q = skip; (q = strstr(q, name)) != NULL; q += n)
    if ((q == skip || q[-1] == ',') && (q[n] == 0 || q[n] == ',')) return true;
  return false;
#endif
}

/* src/platform/pixel.c: the scene's target and the switch to the output's */
int cw__pixel_target(void);
void cw__pixel_ui_begin(void);
/* a gate control (tests/overlay-full-res.mjs): CW_TEST_OVERLAY_SCALED=1 leaves
 * the overlays on the scene's scaled target, as engines before 0.48 did */
static bool overlay_scaled_test(void) {
  const char *v = cw_test_env("CW_TEST_OVERLAY_SCALED");
  return v && atoi(v) != 0;
}

/* what one run of the passes draws: everything (one camera fills the
 * output), one view's 3D picture (no overlays), or only the overlays over
 * the views already laid out */
enum { RUN_FULL, RUN_VIEW, RUN_OVERLAY, RUN_ARRAY }; /* RUN_ARRAY: every view's 3D picture in one run (one-pass split screen) */
static void render_run(cw_tree *t, cw_pipeline *p, int w, int h, double alpha, int mode);

void cw_pipeline_set_views(cw_pipeline *p, const cw_view *views, int n) {
  n = CW_CLAMP(n, 0, CW_MAX_VIEWS);
  if (n != p->nviews) p->taa_reset = true;
  p->nviews = n;
  for (int i = 0; i < n; i++) p->views[i] = views[i];
}
void cw_pipeline_set_views_shared_post(cw_pipeline *p, bool shared) { p->views_shared_post = shared; }
/* splitscreen with a shared post chain: a POST pass belongs to each view
 * when it reads the view's depth or motion, else to the one run over the
 * laid-out views */
static bool post_in_run(const cw_pipeline *p, const pass_rec *r, int mode) {
  if (mode == RUN_ARRAY) mode = RUN_VIEW; /* the views' part of the chain, as each view's run has it */
  if (!p->view_post_split || r->d.phase != CW_PHASE_POST) return true;
  bool per_view = (r->d.reads & (CW_RES_DEPTH | CW_RES_VELOCITY)) != 0;
  return mode == RUN_VIEW ? per_view : !per_view;
}
void cw_pipeline_set_views_array(cw_pipeline *p, bool on) { p->views_array = on; }
void cw__pipeline_set_pass_reads(cw_pipeline *p, const char *name, uint32_t reads) {
  pass_rec *r = find_pass(p, cw_intern(name));
  if (r) r->d.reads = reads;
}
void cw__pipeline_set_pass_views(cw_pipeline *p, const char *name, uint8_t views) {
  pass_rec *r = find_pass(p, cw_intern(name));
  if (r) r->d.views = views;
}
void cw__pipeline_array_refuse(cw_pass_ctx *ctx, const char *why) {
  if (ctx && ctx->view_count > 0 && ctx->pipeline && !ctx->pipeline->arr_refused) ctx->pipeline->arr_refused = cw_intern(why);
}
bool cw_pipeline_views_array_used(cw_pipeline *p, const char **why_not) {
  if (why_not) *why_not = p->arr_used ? NULL : p->arr_why;
  return p->arr_used;
}
int cw_pipeline_views(cw_pipeline *p, const cw_view **views) {
  if (views) *views = p->views;
  return p->nviews >= 2 ? p->nviews : 0;
}
void cw_pipeline_set_view_hook(cw_pipeline *p, void (*hook)(int view, void *user), void *user) {
  p->view_hook = hook;
  p->view_hook_user = user;
}
int cw_pipeline_split(cw_pipeline *p, const cw_node *cameras, int n) {
  cw_view v[CW_MAX_VIEWS];
  n = CW_CLAMP(n, 0, CW_MAX_VIEWS);
  int ww = 0, wh = 0;
  cw_window_size(&ww, &wh);
  for (int i = 0; i < n; i++) {
    v[i].camera = cameras[i];
    if (n == 2 && wh > ww) v[i].x = 0, v[i].y = 0.5f * i, v[i].w = 1, v[i].h = 0.5f;
    else if (n == 2) v[i].x = 0.5f * i, v[i].y = 0, v[i].w = 0.5f, v[i].h = 1;
    else v[i].x = 0.5f * (float)(i & 1), v[i].y = 0.5f * (float)(i / 2), v[i].w = 0.5f, v[i].h = 0.5f;
  }
  cw_pipeline_set_views(p, v, n);
  return n;
}

/* the views laid into the output: one full-screen pass that samples each
 * view's picture inside its rectangle, a thin dark seam between them */
static unsigned views_prog;
static void views_composite(cw_pipeline *p, cw_tree *t, const cw_rt *output, const cw_rt *const *rts, int n) {
  if (!views_prog)
    views_prog = cw__pass_program2("views",
      "uniform sampler2D u_t0;\nuniform sampler2D u_t1;\nuniform sampler2D u_t2;\nuniform sampler2D u_t3;\n"
      "uniform vec4 u_r0;\nuniform vec4 u_r1;\nuniform vec4 u_r2;\nuniform vec4 u_r3;\nuniform vec4 u_misc;\nout vec4 o;\n"
      "bool inr(vec4 r, vec2 q) { return q.x >= r.x && q.y >= r.y && q.x < r.x + r.z && q.y < r.y + r.w; }\n"
      "float seam(vec4 r, vec2 q) { vec2 d = min(q - r.xy, r.xy + r.zw - q) * u_misc.xy; return min(d.x, d.y); }\n"
      "void main() {\n"
      "  vec2 q = v_uv; o = vec4(0.02, 0.018, 0.015, 1.0); float e = 99.0;\n"
      "  if (inr(u_r0, q)) { o = texture(u_t0, (q - u_r0.xy) / u_r0.zw); e = seam(u_r0, q); }\n"
      "  else if (u_misc.z > 1.5 && inr(u_r1, q)) { o = texture(u_t1, (q - u_r1.xy) / u_r1.zw); e = seam(u_r1, q); }\n"
      "  else if (u_misc.z > 2.5 && inr(u_r2, q)) { o = texture(u_t2, (q - u_r2.xy) / u_r2.zw); e = seam(u_r2, q); }\n"
      "  else if (u_misc.z > 3.5 && inr(u_r3, q)) { o = texture(u_t3, (q - u_r3.xy) / u_r3.zw); e = seam(u_r3, q); }\n"
      "  if (e < u_misc.w) o.rgb *= 0.08;\n"
      "  o.a = 1.0;\n}\n",
      "struct U { u_r0: vec4f, u_r1: vec4f, u_r2: vec4f, u_r3: vec4f, u_misc: vec4f };\n"
      "@group(0) @binding(0) var<uniform> u: U;\n"
      "@group(0) @binding(1) var u_t0: texture_2d<f32>;\n@group(0) @binding(2) var u_t1: texture_2d<f32>;\n"
      "@group(0) @binding(3) var u_t2: texture_2d<f32>;\n@group(0) @binding(4) var u_t3: texture_2d<f32>;\n"
      "fn inr(r: vec4f, q: vec2f) -> bool { return q.x >= r.x && q.y >= r.y && q.x < r.x + r.z && q.y < r.y + r.w; }\n"
      "fn seam(r: vec4f, q: vec2f) -> f32 { let d = min(q - r.xy, r.xy + r.zw - q) * u.u_misc.xy; return min(d.x, d.y); }\n"
      "@fragment fn fs(@builtin(position) pos: vec4f, @location(0) uv: vec2f) -> @location(0) vec4f {\n"
      "  let q = uv;\n"
      "  let c0 = textureSampleLevel(u_t0, cw_linear, (q - u.u_r0.xy) / u.u_r0.zw, 0.0);\n"
      "  let c1 = textureSampleLevel(u_t1, cw_linear, (q - u.u_r1.xy) / u.u_r1.zw, 0.0);\n"
      "  let c2 = textureSampleLevel(u_t2, cw_linear, (q - u.u_r2.xy) / u.u_r2.zw, 0.0);\n"
      "  let c3 = textureSampleLevel(u_t3, cw_linear, (q - u.u_r3.xy) / u.u_r3.zw, 0.0);\n"
      "  var o = vec4f(0.02, 0.018, 0.015, 1.0); var e = 99.0;\n"
      "  if (inr(u.u_r0, q)) { o = c0; e = seam(u.u_r0, q); }\n"
      "  else if (u.u_misc.z > 1.5 && inr(u.u_r1, q)) { o = c1; e = seam(u.u_r1, q); }\n"
      "  else if (u.u_misc.z > 2.5 && inr(u.u_r2, q)) { o = c2; e = seam(u.u_r2, q); }\n"
      "  else if (u.u_misc.z > 3.5 && inr(u.u_r3, q)) { o = c3; e = seam(u.u_r3, q); }\n"
      "  if (e < u.u_misc.w) { o = vec4f(o.rgb * 0.08, o.a); }\n"
      "  return vec4f(o.rgb, 1.0);\n}\n");
  if (!views_prog) return;
  cw_pass_ctx ctx;
  memset(&ctx, 0, sizeof ctx);
  ctx.tree = t;
  ctx.pipeline = p;
  ctx.settings = &p->s;
  ctx.width = output->w;
  ctx.height = output->h;
  ctx.output = output;
  cw_pass_bind_target(&ctx, output);
  cw__pass_state_reset();
  cw_pass_use(views_prog);
  static const char *const T[4] = { "u_t0", "u_t1", "u_t2", "u_t3" }, *const R[4] = { "u_r0", "u_r1", "u_r2", "u_r3" };
  for (int i = 0; i < 4; i++) {
    int k = i < n ? i : 0;
    cw_pass_bind_texture(i, rts[k]->tex);
    cw_pass_uniform1i(views_prog, T[i], i);
    const cw_view *v = &p->views[k];
    /* uv space: (0, 0) at the bottom left */
    cw_pass_uniform4f(views_prog, R[i], v->x, 1 - v->y - v->h, v->w, v->h);
  }
  cw_pass_uniform4f(views_prog, "u_misc", (float)output->w, (float)output->h, (float)n, 1.5f);
  int ts = step_begin(p, "views");
  cw_pass_fullscreen();
  step_end(ts);
}

/* one-pass split screen: the views already lie in one picture; laid into the output as views_composite lays the
 * per-view pictures (the same rectangles, the same seam and the same colour where no view is), from that one
 * texture */
static unsigned views_arr_prog;
static void views_composite_array(cw_pipeline *p, cw_tree *t, const cw_rt *output, const cw_rt *pic, int n) {
  if (!views_arr_prog)
    views_arr_prog = cw__pass_program2("views_array",
      "uniform sampler2D u_t0;\nuniform vec4 u_r0;\nuniform vec4 u_r1;\nuniform vec4 u_r2;\nuniform vec4 u_r3;\nuniform vec4 u_misc;\nout vec4 o;\n"
      "bool inr(vec4 r, vec2 q) { return q.x >= r.x && q.y >= r.y && q.x < r.x + r.z && q.y < r.y + r.w; }\n"
      "float seam(vec4 r, vec2 q) { vec2 d = min(q - r.xy, r.xy + r.zw - q) * u_misc.xy; return min(d.x, d.y); }\n"
      "void main() {\n"
      "  vec2 q = v_uv; o = vec4(0.02, 0.018, 0.015, 1.0); float e = 99.0; vec4 c = texelFetch(u_t0, ivec2(gl_FragCoord.xy), 0);\n"
      "  if (inr(u_r0, q)) { o = c; e = seam(u_r0, q); }\n"
      "  else if (u_misc.z > 1.5 && inr(u_r1, q)) { o = c; e = seam(u_r1, q); }\n"
      "  else if (u_misc.z > 2.5 && inr(u_r2, q)) { o = c; e = seam(u_r2, q); }\n"
      "  else if (u_misc.z > 3.5 && inr(u_r3, q)) { o = c; e = seam(u_r3, q); }\n"
      "  if (e < u_misc.w) o.rgb *= 0.08;\n"
      "  o.a = 1.0;\n}\n",
      "struct U { u_r0: vec4f, u_r1: vec4f, u_r2: vec4f, u_r3: vec4f, u_misc: vec4f };\n"
      "@group(0) @binding(0) var<uniform> u: U;\n"
      "@group(0) @binding(1) var u_t0: texture_2d<f32>;\n"
      "fn inr(r: vec4f, q: vec2f) -> bool { return q.x >= r.x && q.y >= r.y && q.x < r.x + r.z && q.y < r.y + r.w; }\n"
      "fn seam(r: vec4f, q: vec2f) -> f32 { let d = min(q - r.xy, r.xy + r.zw - q) * u.u_misc.xy; return min(d.x, d.y); }\n"
      "@fragment fn fs(@builtin(position) pos: vec4f, @location(0) uv: vec2f) -> @location(0) vec4f {\n"
      "  let q = uv;\n"
      "  let c = textureLoad(u_t0, vec2i(floor(q * u.u_misc.xy)), 0);\n"
      "  var o = vec4f(0.02, 0.018, 0.015, 1.0); var e = 99.0;\n"
      "  if (inr(u.u_r0, q)) { o = c; e = seam(u.u_r0, q); }\n"
      "  else if (u.u_misc.z > 1.5 && inr(u.u_r1, q)) { o = c; e = seam(u.u_r1, q); }\n"
      "  else if (u.u_misc.z > 2.5 && inr(u.u_r2, q)) { o = c; e = seam(u.u_r2, q); }\n"
      "  else if (u.u_misc.z > 3.5 && inr(u.u_r3, q)) { o = c; e = seam(u.u_r3, q); }\n"
      "  if (e < u.u_misc.w) { o = vec4f(o.rgb * 0.08, o.a); }\n"
      "  return vec4f(o.rgb, 1.0);\n}\n");
  if (!views_arr_prog) return;
  cw_pass_ctx ctx;
  memset(&ctx, 0, sizeof ctx);
  ctx.tree = t;
  ctx.pipeline = p;
  ctx.settings = &p->s;
  ctx.width = output->w;
  ctx.height = output->h;
  ctx.output = output;
  cw_pass_bind_target(&ctx, output);
  cw__pass_state_reset();
  cw_pass_use(views_arr_prog);
  cw_pass_bind_texture(0, pic->tex);
  cw_pass_uniform1i(views_arr_prog, "u_t0", 0);
  static const char *const R[4] = { "u_r0", "u_r1", "u_r2", "u_r3" };
  for (int i = 0; i < 4; i++) {
    const cw_view *v = &p->views[i < n ? i : 0];
    cw_pass_uniform4f(views_arr_prog, R[i], v->x, 1 - v->y - v->h, v->w, v->h);
  }
  cw_pass_uniform4f(views_arr_prog, "u_misc", (float)output->w, (float)output->h, (float)n, 1.5f);
  int ts = step_begin(p, "views");
  cw_pass_fullscreen();
  step_end(ts);
}

/* One-pass split screen: every view's 3D picture in one run over one full-size target, then laid out with the
 * same seams as the per-view runs. NULL (nothing drawn) when a camera is missing or an active pass cannot. */
static const cw_rt *array_run(cw_tree *t, cw_pipeline *p, const cw_rt *out, double alpha, cw_rt_format vf) {
  int W = out->w, H = out->h, n = p->nviews;
  p->arr_why = NULL;
  for (int k = 0; k < n; k++)
    if (!cw_node_valid(p->views[k].camera)) return NULL;
  cw_render3d_set_camera(t, p->views[0].camera);
  cw__render3d_keep_shadows(t, false);
  if (!cw_render3d_prepare(t, W, H)) return NULL; /* the renderer at the whole target's size; the shadow maps once */
  if (!p->arr_cam) p->arr_cam = t3_array_camera_new();
  uint32_t arr_cam_layers[CW_MAX_VIEWS];
  bool arr_cam_vl[CW_MAX_VIEWS];
  for (int i = 0; i < p->arr_cam->count; i++) t3_release(p->arr_cam->cameras[i]);
  p->arr_cam->count = 0;
  for (int k = 0; k < n; k++) {
    const cw_view *v = &p->views[k];
    int vw = CW_MAX(1, (int)(v->w * (float)W + 0.5f)), vh = CW_MAX(1, (int)(v->h * (float)H + 0.5f));
    int x0 = (int)(v->x * (float)W + 0.5f), ytop = (int)(v->y * (float)H + 0.5f);
    p->arr_nodes[k] = v->camera;
    p->arr_cams[k] = cw__render3d_view_camera(v->camera, vw, vh);
    if (!p->arr_cams[k]) return NULL;
    /* the geometry drawn for this view only (src/render/view_layers.h): its layer, where a view drawn alone sees
     * layer 0 (the camera's other layers as they are) */
    uint32_t L = p->arr_cams[k]->base.layers & ~CW__VIEW_LAYERS;
    p->arr_cams[k]->base.layers = L | ((p->arr_cams[k]->base.layers & 1u) ? cw__view_layer(k) : 0);
    arr_cam_layers[k] = L;
    arr_cam_vl[k] = (L & 1u) != 0;
    int r[4] = { x0, H - ytop - vh, vw, vh };
    memcpy(p->arr_rect[k], r, sizeof r);
    /* (three.c places a viewport from the target's top row; the WebGPU scene target is drawn flipped, its rows
     * bottom-up as GL's, so there the top row of the picture is the target's last) */
    int vp[4] = { x0, cw_gpu_backend() == CW_GPU_WEBGPU ? r[1] : ytop, vw, vh };
    memcpy(p->arr_vp[k], vp, sizeof vp);
    t3_array_camera_add(p->arr_cam, p->arr_cams[k], vp[0], vp[1], vp[2], vp[3]);
  }
  p->arr_n = n;
  const cw_rt *pic = cw_pipeline_rt_persistent(p, "views.arr", W, H, vf, NULL);
  if (!pic) return NULL;
  const cw_rt *keep_out = p->output_user;
  p->output_user = pic;
  p->view_i = -1;
  p->view_pic = NULL;
  p->arr_refused = NULL;
  /* the objects drawn in some views only (cw_object_set_views): on those views' layers for the run */
  const char *mask_why = cw__view_masks_apply_layers(n, arr_cam_layers, arr_cam_vl);
  if (mask_why) {
    p->output_user = keep_out;
    p->arr_why = cw_intern(mask_why);
    return NULL;
  }
  render_run(t, p, W, H, alpha, RUN_ARRAY);
  cw__visibility_end_view(t);
  cw__view_masks_restore_layers();
  p->output_user = keep_out;
  if (p->arr_refused) {
    p->arr_why = p->arr_refused;
    return NULL;
  }
  return p->view_pic ? p->view_pic : pic;
}

/* ── backdrops (cartwheel/backdrop.h) ────────────────────────────────── */

const cw_rt *cw__pipeline_render_backdrop(cw_tree *world, cw_node camera, int w, int h, double alpha) {
  cw_pipeline *p = cw_pipeline_of(world);
  bool keep_taa = p->s.taa, keep_split = p->view_post_split;
  cw_node keep_cam = cw__render3d_explicit_camera(world);
  p->s.taa = false;          /* (its history would be its camera's; the main tree's TAA runs over the composite) */
  p->view_post_split = true; /* the depth-reading post passes here, the rest in the main tree's chain */
  p->view_i = -1;
  p->view_pic = NULL;
  p->bd_pic = NULL;
  p->backdrop_run = true;
  if (cw_node_valid(camera)) cw_render3d_set_camera(world, camera);
  cw_pipeline_render(world, w, h, alpha);
  if (cw_node_valid(camera)) cw_render3d_set_camera(world, keep_cam);
  p->backdrop_run = false;
  p->s.taa = keep_taa;
  p->view_post_split = keep_split;
  return p->bd_pic;
}

/* ── extra views ─────────────────────────────────────────────────────── */

bool cw__pipeline_queue_view(cw_pipeline *p, cw_node camera, const cw_rt *rt, int slot, void (*done)(void *user, float cpu_ms), void *user) {
  if (p->nextra >= (int)CW_ARRAY_LEN(p->extra)) return false;
  p->extra[p->nextra++] = (struct cw__extra_view){ camera, rt, slot, done, user };
  return true;
}

/* The queued views, after the frame's own: each through its camera into its target with the whole pipeline but
 * the overlays, as a later splitscreen view is drawn (the frame's shadow maps, light clusters and probe captures
 * are reused, TAA is off: its history is the main camera's). What the frame's own views drew and will draw next
 * frame is untouched: the output, the camera, the previous view-projection and every persistent target. */
static void extra_views_run(cw_tree *t, cw_pipeline *p, double alpha) {
  if (!p->nextra) return;
  bool wgpu = cw_gpu_backend() == CW_GPU_WEBGPU;
  const cw_rt *keep_out = p->output_user;
  bool keep_taa = p->s.taa, keep_have = p->have_prev, keep_split = p->view_post_split;
  cw_node keep_cam = cw__render3d_explicit_camera(t);
  int base = p->nviews >= 2 ? p->nviews : 1;
  p->s.taa = false;
  p->view_post_split = false;
  if (wgpu) cw__pw_timing_hold(true);
  int n = p->nextra;
  p->nextra = 0; /* a view queued while these run waits for the next frame */
  for (int i = 0; i < n; i++) {
    struct cw__extra_view e = p->extra[i];
    double t0 = cw__render3d_now_ms();
    if (e.rt && cw_node_valid(e.camera)) {
      char scope[16];
      snprintf(scope, sizeof scope, "x%d", e.slot);
      p->rt_scope = cw_intern(scope);
      p->output_user = e.rt;
      cw_render3d_set_camera(t, e.camera);
      cw__render3d_keep_shadows(t, true);
      p->view_i = base + i; /* a later view: past CW_MAX_VIEWS it has no per-view slot (volumetric fog skips it) */
      p->view_pic = NULL;
      render_run(t, p, e.rt->w, e.rt->h, alpha, RUN_VIEW);
      cw__visibility_end_view(t);
    }
    if (e.done) e.done(e.user, (float)(cw__render3d_now_ms() - t0));
  }
  if (wgpu) cw__pw_timing_hold(false);
  p->rt_scope = NULL;
  p->view_i = -1;
  cw__render3d_keep_shadows(t, false);
  cw_render3d_set_camera(t, keep_cam);
  p->output_user = keep_out;
  p->s.taa = keep_taa;
  p->have_prev = keep_have;
  p->view_post_split = keep_split;
}

void cw_pipeline_render(cw_tree *t, int w, int h, double alpha) {
  CW_PROFILE_SCOPE("pipeline.render");
  cw_pipeline *p = cw_pipeline_of(t);
  bool wgpu = cw_gpu_backend() == CW_GPU_WEBGPU;
  cw__pipeline_probe_caps();
  /* backdrops (cartwheel/backdrop.h): their worlds drawn into their pictures first, each through its own pipeline */
  p->bd_n = 0;
  p->bd_env_n = 0;
  p->bd_env_warm_n = 0;
  p->post_warm_ran = false;
  if (p->bd_frame && !p->backdrop_run) p->bd_frame(t, p, w, h, alpha);
  if (wgpu) {
    cw__pw_frame_begin();
    cw__pw_timing_poll();
  }
  cw__lighting_frame_begin(t); /* (may add the lighting pass) */
  if (p->order_dirty) {
    qsort(p->passes.data, (size_t)p->passes.count, sizeof *p->passes.data, pass_cmp);
    p->order_dirty = false;
  }
  p->run = 0;
  p->msaa_resolves = 0;
  cw__render3d_frame_begin(t); /* the frame's draw counts start over */
  cw__view_masks_frame();
  views_submits_dbg = 0;
  if (p->backdrop_run) { /* a backdrop's picture: one view of a shared post chain's kind, the chain the main tree's */
    render_run(t, p, w, h, alpha, RUN_VIEW);
    cw__visibility_end_view(t);
    frame_stats(p);
    p->frame++;
    return;
  }
  if (p->nviews < 2) {
    render_run(t, p, w, h, alpha, RUN_FULL);
    extra_views_run(t, p, alpha);
    frame_stats(p);
    p->frame++;
    return;
  }
  /* splitscreen: each view's picture into its own target, through its own
   * camera, without TAA (its history belongs to one camera) */
  const cw_rt *keep_out = p->output_user;
  p->screen = (cw_rt){ .fbo = cw__gl_window_fbo, .w = w, .h = h, .format = CW_RT_RGBA8 }; /* the window (pixel.c may stand in) */
  const cw_rt *output = keep_out ? keep_out : &p->screen;
  cw_rt out_copy = *output;
  bool keep_taa = p->s.taa;
  p->s.taa = false;
  /* a shared post chain: the views keep HDR pictures for it */
  p->view_post_split = p->views_shared_post;
  cw_rt_format vf = p->view_post_split && p->s.hdr && cw__caps.float_render ? hdr_format() : CW_RT_RGBA8;
  if (wgpu) cw__pw_timing_hold(true);
  const cw_rt *rts[CW_MAX_VIEWS];
  int n = 0;
  /* one-pass split screen when every pass that draws a view's part can (else each view on its own) */
  const cw_rt *arr_pic = NULL;
  p->arr_used = false;
  bool want_array = p->views_array;
  const char *va = cw_test_env("CW_TEST_VIEWS_ARRAY"); /* 1 / 0: one pass or per view whatever the app set (gates) */
  if (va && *va) want_array = atoi(va) != 0;
  const char *vs = cw_test_env("CW_TEST_VIEWS_SUBMIT"); /* 0: the views recorded first, then sent to the GPU (gates) */
  if (vs && *vs) p->views_submit = atoi(vs) != 0;
  if (want_array && p->view_post_split && !p->view_hook) {
    arr_pic = array_run(t, p, &out_copy, alpha, vf);
    p->arr_used = arr_pic != NULL;
  } else p->arr_why = NULL;
  for (int i = 0; i < p->nviews && !arr_pic; i++) {
    const cw_view *v = &p->views[i];
    int vw = CW_MAX(1, (int)(v->w * (float)out_copy.w + 0.5f)), vh = CW_MAX(1, (int)(v->h * (float)out_copy.h + 0.5f));
    char key[16];
    snprintf(key, sizeof key, "view%d", i);
    const cw_rt *rt = cw_pipeline_rt_persistent(p, key, vw, vh, vf, NULL);
    if (!rt || !cw_node_valid(v->camera)) continue;
    p->output_user = rt;
    cw_render3d_set_camera(t, v->camera);
    if (p->view_hook) p->view_hook(i, p->view_hook_user);
    cw__view_masks_apply_visible(i); /* (cw_object_set_views) */
    cw__render3d_keep_shadows(t, n > 0); /* the shadow maps once a frame: the first view draws them */
    p->view_i = i;
    p->view_pic = NULL;
    render_run(t, p, vw, vh, alpha, RUN_VIEW);
    cw__visibility_end_view(t);
    rts[n++] = p->view_pic ? p->view_pic : rt;
  }
  cw__view_masks_restore_visible();
  p->view_i = -1;
  cw__render3d_keep_shadows(t, false);
  cw_render3d_set_camera(t, (cw_node){ 0 });
  p->output_user = keep_out;
  p->s.taa = keep_taa;
  /* shared post: the views laid out into one HDR picture, the chain's input */
  const cw_rt *laid = p->view_post_split && (n || arr_pic) ? cw_pipeline_rt_persistent(p, "views.post", out_copy.w, out_copy.h, vf, NULL) : NULL;
  if (arr_pic) views_composite_array(p, t, laid ? laid : &out_copy, arr_pic, p->nviews);
  else if (n) views_composite(p, t, laid ? laid : &out_copy, rts, n);
  p->post_src = laid;
  /* the overlays once, over the whole output (the HUD reads cw_pipeline_views);
   * WebGPU reads the frame's pass timestamps back after it, not per view */
  if (wgpu) cw__pw_timing_hold(false);
  render_run(t, p, w, h, alpha, RUN_OVERLAY);
  p->post_src = NULL;
  p->view_post_split = false;
  extra_views_run(t, p, alpha);
  frame_stats(p);
  p->frame++;
}

static void render_run_inner(cw_tree *t, cw_pipeline *p, int w, int h, double alpha, int mode);
/* one-pass split screen: the context of view k (its camera, matrices, history and rectangle; the pass helpers
 * scissor to it), and back to the run's own after the views */
int cw__array_view_now = -1;
static void array_view(cw_pass_ctx *ctx, cw_pipeline *p, int k) {
  cw__array_view_now = k;
  ctx->camera = p->arr_cams[k];
  ctx->camera_node = p->arr_nodes[k];
  mat4_from_camera(ctx);
  const struct cw__view_prev *vp = &p->view_prev[k];
  ctx->prev_view_proj = vp->have && vp->w == p->arr_rect[k][2] && vp->h == p->arr_rect[k][3] ? vp->vp : ctx->view_proj;
  ctx->view_index = k;
  memcpy(ctx->view_rect, p->arr_rect[k], sizeof ctx->view_rect);
  /* a gate control (CW_TEST_VIEWS_ARRAY_SHIFT=1): each view's passes draw a rectangle off by its width (its height
   * for a view as wide as the target; wrapped round the target), the scene where it belongs */
  static int shift = -1;
  if (shift < 0) {
    const char *e = cw_test_env("CW_TEST_VIEWS_ARRAY_SHIFT");
    shift = e && *e ? atoi(e) != 0 : 0;
    if (!e) shift = -1; /* (not read yet on a cart's first frame: ask again) */
  }
  if (shift > 0 && ctx->view_rect[2] < ctx->width) ctx->view_rect[0] = (ctx->view_rect[0] + ctx->view_rect[2]) % CW_MAX(ctx->width, 1);
  else if (shift > 0) ctx->view_rect[1] = (ctx->view_rect[1] + ctx->view_rect[3]) % CW_MAX(ctx->height, 1);
  cw__vclip.on = true;
  memcpy(cw__vclip.rect, ctx->view_rect, sizeof cw__vclip.rect);
  cw__vclip.full_w = ctx->width;
  cw__vclip.full_h = ctx->height;
  if (cw_gpu_backend() != CW_GPU_WEBGPU) { /* (each view's pass starts from the whole target, as a lone view's) */
    glViewport(0, 0, cw__vclip.bound_w, cw__vclip.bound_h);
    glDisable(GL_SCISSOR_TEST);
  }
}
static void array_view_end(cw_pass_ctx *ctx, cw_pipeline *p) {
  cw__array_view_now = -1;
  cw__vclip.on = false;
  if (cw_gpu_backend() != CW_GPU_WEBGPU) {
    glViewport(0, 0, cw__vclip.bound_w, cw__vclip.bound_h);
    glDisable(GL_SCISSOR_TEST);
  }
  ctx->view_index = -1;
  ctx->camera = p->arr_cams[0];
  ctx->camera_node = p->arr_nodes[0];
  mat4_from_camera(ctx);
  ctx->prev_view_proj = ctx->view_proj;
}

/* one-pass split screen: before view k's shadow maps, the shadow casters a frame drawn view by view has for view k (its
 * masked objects, cw_object_set_views, and the Shadows setting's small casters culled from its camera) for the
 * clustered lights' shadow atlas; view 0's from the run's start and for three.c's own shadow maps, which a frame drawn
 * view by view draws once, in its first view. CW_TEST_VIEWS_SHARED_CASTERS=1 (a gate's control): view 0's for every view, as before v0.57.4. */
static int arr_casters_view;
static void array_casters(cw_pass_ctx *ctx, cw_pipeline *p, int k) {
  static int shared = -1;
  if (shared < 0) {
    const char *e = cw_test_env("CW_TEST_VIEWS_SHARED_CASTERS");
    shared = e && *e ? atoi(e) != 0 : 0;
    if (!e) shared = -1; /* (not read yet on a cart's first frame: ask again) */
  }
  if (shared > 0 || arr_casters_view == k) return;
  arr_casters_view = k;
  cw__shadow_cull_end();
  cw__view_masks_cast_for_view(k);
  if (ctx->scene3d) cw__shadow_cull_begin(ctx->scene3d, p->arr_cams[k]);
}

/* adds the lines of add (one program state a line) not yet in text; returns the lines text holds. text keeps every
 * tree's states for the run: each is a state the app's tables lack */
static int32_t missing_union(char *text, size_t cap, const char *add) {
  for (const char *a = add; a && *a;) {
    const char *e = strchr(a, '\n');
    size_t l = e ? (size_t)(e - a) : strlen(a);
    bool have = false;
    for (const char *t = text; *t && !have;) {
      const char *te = strchr(t, '\n');
      size_t tl = te ? (size_t)(te - t) : strlen(t);
      have = tl == l && !memcmp(t, a, l);
      t += tl + (te ? 1 : 0);
    }
    size_t n = strlen(text);
    if (!have && l && n + l + 1 < cap) {
      memcpy(text + n, a, l);
      text[n + l] = '\n';
      text[n + l + 1] = 0;
    }
    a += l + (e ? 1 : 0);
  }
  int32_t c = 0;
  for (const char *t = text; *t; t++) c += *t == '\n' || !t[1];
  return c;
}

/* a view's run, its shadow caster culling (src/render/shadow_cull.c) undone after it */
static void render_run(cw_tree *t, cw_pipeline *p, int w, int h, double alpha, int mode) {
  render_run_inner(t, p, w, h, alpha, mode);
  cw__shadow_cull_end();
}

static void render_run_inner(cw_tree *t, cw_pipeline *p, int w, int h, double alpha, int mode) {
  double t_start = cw__render3d_now_ms(); /* cw_render3d_stats.cpu_ms: up to the overlay phase */
  bool wgpu = cw_gpu_backend() == CW_GPU_WEBGPU;
  p->bd_drawn = false;
  /* the frame's settings: the pipeline's, capped or raised by the quality
   * preset (cartwheel/quality.h; High = as authored) */
  cw_post_settings eff = p->s;
  cw__quality_post(&eff);
  cw_post_settings *s = &eff;

  /* the output */
  p->screen = (cw_rt){ .fbo = cw__gl_window_fbo, .w = w, .h = h, .format = CW_RT_RGBA8 }; /* the window (pixel.c may stand in) */
  const cw_rt *output = p->output_user ? p->output_user : &p->screen;

  cw_pass_ctx ctx;
  memset(&ctx, 0, sizeof ctx);
  ctx.tree = t;
  ctx.pipeline = p;
  ctx.settings = s;
  ctx.width = w;
  ctx.height = h;
  ctx.frame = p->frame;
  ctx.time = cw_time();
  ctx.alpha = alpha;
  ctx.output = output;
  /* a tree with no Camera3D (2D only) gets no three.c state at all. On
   * WebGPU three.c renders with its WebGPU backend (cw_render3d_prepare
   * made the renderer with t3_renderer_use_wgpu) */
  CW_PROFILE_BEGIN("render3d.prepare");
  ctx.camera = mode == RUN_OVERLAY ? NULL : mode == RUN_ARRAY ? p->arr_cams[0] : cw_render3d_prepare(t, w, h);
  if (mode == RUN_ARRAY) ctx.view_count = p->arr_n, ctx.view_index = -1;
  CW_PROFILE_END();
  if (ctx.camera) {
    ctx.scene3d = cw_render3d_scene(t);
    ctx.camera_node = cw_render3d_camera(t);
    ctx.renderer = cw_render3d_renderer(t);
#ifdef T3_MISSING_HOOK
    if (ctx.renderer) t3_renderer_set_missing_hook(ctx.renderer, missing_hook, t); /* names a draw with no program */
#endif
#ifdef T3_EXTENSION_MISS_HOOK
    if (ctx.renderer) t3_renderer_set_extension_miss_hook(ctx.renderer, ext_miss_hook, NULL); /* names a value a program lacks */
#endif
    if (ctx.scene3d) cw__shadow_cull_begin(ctx.scene3d, ctx.camera); /* the Shadows setting: small casters off for this run */
    arr_casters_view = 0;
  }
  ctx.hdr = s->hdr && cw__caps.float_render;

  /* the post prewarm (cw_post_prewarm): in a render of the whole scene (not a warm-up slice, a backdrop's world or an
   * extra view), the passes its settings turn on and these do not run once each, in the shadow and post phases, with
   * those settings and their picture kept out of the frame: what the frame shows is what it shows without them */
  bool whole = ctx.camera && !p->backdrop_run && !p->rt_scope && !cw__warm_partial &&
               (mode == RUN_FULL || mode == RUN_ARRAY || (mode == RUN_VIEW && p->view_i == 0));
  bool post_warm = whole && p->post_warm_armed && p->post_warm_have, scene_warm = whole && p->scene_warm_armed && p->post_warm_have;
  cw_post_settings warm_s;
  if (post_warm) {
    warm_s = p->post_warm_s;
    cw__quality_post(&warm_s);
  }
  int gi_tagged0 = post_warm ? cw__gi_tagged(t) : 0;
  bool any_warm = false;
  /* which passes run: settings-driven ones decide in active() */
  uint32_t reads = 0, warm_reads = 0;
  bool post_or_fx = false, any_prepass = false, wgpu_opaque = false;
  pass_rec *velocity = NULL;
  for (int i = 0; i < p->passes.count; i++) {
    pass_rec *r = p->passes.data[i];
    if (p->run == 0) { /* a frame's first run: its stats start over (the views' runs add up) */
      r->stat.ran = r->stat.fused = false;
      r->frame_cpu = 0;
    }
    r->warm_only = false;
    if (r->name == cw_intern("velocity")) {
      velocity = r;
      continue;
    }
    r->active = r->enabled && !test_skipped(r->name) && (!r->d.active || r->d.active(&ctx, r->d.user));
    if (!r->active && post_warm && r->enabled && r->d.active && !test_skipped(r->name) && (!wgpu || r->d.webgpu) &&
        (r->d.phase == CW_PHASE_SHADOW || r->d.phase == CW_PHASE_POST)) {
      ctx.settings = &warm_s;
      r->warm_only = r->d.active(&ctx, r->d.user);
      ctx.settings = s;
      if (r->warm_only) warm_reads |= r->d.reads, any_warm = true;
    }
    if (r->active && wgpu && !r->d.webgpu) {
      r->active = false;
      if (!r->wgpu_warned) {
        r->wgpu_warned = true;
        if (r->name == cw_intern("scene"))
          cw_error("render pipeline: the scene pass is off on WebGPU");
        else cw_error("render pipeline: pass '%s' has no WebGPU path; it is skipped on WebGPU", r->name);
      }
    }
    if (!r->active) continue;
    if (r->d.phase == CW_PHASE_OPAQUE) wgpu_opaque = true;
    if (r->d.phase != CW_PHASE_OVERLAY && r->d.phase != CW_PHASE_PRESENT) reads |= r->d.reads;
    if (r->d.phase == CW_PHASE_SKY || r->d.phase == CW_PHASE_EFFECTS || r->d.phase == CW_PHASE_POST) post_or_fx = true;
    if (r->d.phase == CW_PHASE_PREPASS) any_prepass = true;
  }
  if (s->debug_view == CW_VIEW_VELOCITY) reads |= CW_RES_VELOCITY;
  p->defer_trans = active_index(p, "transparents") >= 0; /* (the scene pass draws the opaque queue alone) */
  if (velocity) velocity->active = velocity->enabled && !test_skipped(velocity->name) && (reads & CW_RES_VELOCITY) != 0;
  /* one-pass split screen: every pass that draws a view's part must take the views over one target; the first that
   * cannot sends the frame back to the per-view runs (nothing drawn yet) */
  if (mode == RUN_ARRAY) {
    for (int i = 0; i < p->passes.count; i++) {
      pass_rec *r = p->passes.data[i];
      if (!r->active || r->d.phase >= CW_PHASE_OVERLAY || !post_in_run(p, r, mode)) continue;
      if (r->d.views == CW_PASS_VIEWS_NONE) {
        p->arr_refused = r->name;
        return;
      }
    }
  }
  /* screenshot by layer (cartwheel/debug.h): without WORLD no 3D phase runs */
  uint32_t layers = cw_render_layers();
  bool has_scene = ctx.camera && (ctx.renderer != NULL || (wgpu && wgpu_opaque)) && (layers & CW_LAYER_WORLD);
  p->drew3d = mode == RUN_OVERLAY; /* the views' pictures are already there: the overlays keep them */
  /* WebGPU always renders the scene offscreen: the window texture has no
   * depth */
  bool offscreen = has_scene && (wgpu || post_or_fx || s->hdr || s->msaa > 1 || any_prepass || p->backdrop_run ||
                                 (reads & (CW_RES_DEPTH | CW_RES_VELOCITY)));
  /* splitscreen's shared post: the overlay run starts from the laid-out views */
  bool post_run = mode == RUN_OVERLAY && p->post_src;
  if (post_run) offscreen = true;
  ctx.offscreen = offscreen;
  /* (a view of split screen's shared post chain is not its own: the tonemap pass runs over the laid-out views, so a
   * height fog fused into it would be drawn by no run) */
  plan_fusion(p, s, offscreen, ctx.camera != NULL && !post_run && !(p->view_post_split && (mode == RUN_VIEW || mode == RUN_ARRAY)));

  /* TAA jitter + camera matrices (offscreen only: the direct path leaves the
   * camera exactly as three.c's own path does) */
  bool taa = offscreen && s->taa && !post_run;
  if (offscreen && !post_run) {
    mat4_from_camera(&ctx);
    if (mode == RUN_ARRAY) ctx.prev_view_proj = ctx.view_proj; /* each view's own, as its passes run (array_view) */
    else if (mode == RUN_VIEW && p->view_i >= CW_MAX_VIEWS) ctx.prev_view_proj = ctx.view_proj; /* an extra view past the view slots: no history */
    else if (mode == RUN_VIEW && p->view_i >= 0) {
      const struct cw__view_prev *vp = &p->view_prev[p->view_i];
      ctx.prev_view_proj = vp->have && vp->w == w && vp->h == h ? vp->vp : ctx.view_proj;
    } else if (!p->have_prev || p->prev_w != w || p->prev_h != h) ctx.prev_view_proj = ctx.view_proj;
    else ctx.prev_view_proj = p->prev_view_proj;
    if (taa) {
      int n = CW_CLAMP(s->taa_samples, 1, 64);
      int k = (int)(p->frame % (uint64_t)n) + 1;
      ctx.jitter[0] = halton(k, 2) - 0.5f;
      ctx.jitter[1] = halton(k, 3) - 0.5f;
      t3_camera *c = ctx.camera;
      c->projection_matrix.e[8] += ctx.jitter[0] * 2.0f / (float)w;
      c->projection_matrix.e[9] += ctx.jitter[1] * 2.0f / (float)h;
    }
    /* the jittered inverse (the camera's own is kept unjittered for raycasts) */
    t3_mat4_invert(&p->inv_proj_jittered, &ctx.camera->projection_matrix);
    t3_mat4 vp_j;
    t3_mat4_multiply(&vp_j, &ctx.camera->projection_matrix, &ctx.view);
    t3_mat4_invert(&p->inv_view_proj_jittered, &vp_j);
  }

  /* three.c: scene-linear output into the HDR target (r187 programs, physical lights, colour
   * management); exposure, the tone curve and the sRGB transfer are the tonemap pass's */
  if (ctx.renderer) {
    t3_renderer_set_tone_mapping(ctx.renderer, T3_NO_TONE_MAPPING, 1);
    t3_renderer_set_output_color_space(ctx.renderer, T3_LINEAR_SRGB_COLOR_SPACE);
  }

  /* WebGPU: passes sample an R32F linear depth copied after the opaque phase */
  bool wgpu_depth = wgpu && p->fuse_fog_tm; /* (the tonemap pass reads it for the fused fog) */
  for (int i = 0; wgpu && i < p->passes.count; i++) {
    pass_rec *r = p->passes.data[i];
    if (r->active && r->d.phase >= CW_PHASE_SKY && r->d.phase <= CW_PHASE_POST && (r->d.reads & (CW_RES_DEPTH | CW_RES_VELOCITY)))
      wgpu_depth = true;
  }
  /* a post prewarm's pass that samples it gets the linear depth too (drawn into a target of its own: nothing else the
   * frame decides changes, its multisampling included) */
  bool warm_depth = wgpu && any_warm && (warm_reads & (CW_RES_DEPTH | CW_RES_VELOCITY));
  bool fx_any = false;
  for (int i = 0; i < p->passes.count; i++) {
    pass_rec *r = p->passes.data[i];
    if (r->active && (r->d.phase == CW_PHASE_SKY || r->d.phase == CW_PHASE_EFFECTS)) fx_any = true;
  }
  /* the scene target */
  const cw_rt *scene = post_run ? p->post_src : NULL, *scene_draw = NULL;
  cw_rt msaa_gl;
  if (offscreen && !post_run) {
    cw_rt_format sf = ctx.hdr ? hdr_format() : CW_RT_RGBA8;
    /* a shared post chain's views each keep their own scene target: it is
     * the view's picture, laid out as it is */
    char skey[24] = "scene";
    if (mode == RUN_VIEW && p->view_post_split && p->view_i >= 0) snprintf(skey, sizeof skey, "scene.v%d", p->view_i);
    scene = persistent_depth(p, skey, w, h, sf);
    if (!scene) offscreen = ctx.offscreen = false;
    else {
      scene_draw = scene;
      static bool msaa_warned;
      /* core WebGPU multisamples any scene: the colour resolves at the end of
       * each render pass, the depth when the opaque phase ends (below).
       * Compatibility mode (the tier's floor, a device without core):
       * RGBA16Float cannot be multisampled, and a multisampled depth cannot be
       * read back (no textureLoad on depth, no sampling of multisampled
       * textures), so 4x MSAA serves RGBA8 scenes that no SKY / EFFECTS /
       * depth-reading pass follows; anything else renders single-sampled
       * (logged once). CW_TEST_WGPU_NO_MSAA=1 (native test runs) renders every
       * WebGPU scene single-sampled: the control the MSAA parity gates must fail. */
      bool core = wgpu && cw__pw_core();
      bool msaa_ok = core || (sf == CW_RT_RGBA8 && !wgpu_depth && !fx_any);
#if !CW_CART
      bool no_msaa_test = wgpu && getenv("CW_TEST_WGPU_NO_MSAA") && atoi(getenv("CW_TEST_WGPU_NO_MSAA"));
      if (no_msaa_test) msaa_ok = false;
#else
      const bool no_msaa_test = false;
#endif
      if (s->msaa > 1 && wgpu && !msaa_ok && !msaa_warned) {
        msaa_warned = true;
        if (no_msaa_test) cw_warn("CW_TEST_WGPU_NO_MSAA: the WebGPU scene renders without MSAA (a gate control)");
        else cw_warn("render pipeline: MSAA on WebGPU in compatibility mode needs an RGBA8 scene (hdr off) and no SKY / EFFECTS / "
                "depth-reading pass; this frame renders without MSAA (a core WebGPU device multisamples any scene)");
      }
      if (s->msaa > 1 && wgpu && msaa_ok && !test_gl_msaa_on_webgpu) { /* the control takes the GL path even where WebGPU could multisample (a core device) */
        /* 4x: WebGPU's only multisample count (2 and 8 become 4), resolved by each render pass */
        if (!p->msaa_rt.fbo || p->msaa_rt.w != w || p->msaa_rt.h != h || p->msaa_rt.format != sf) {
          if (p->msaa_rt.fbo) cw__rt_destroy(&p->msaa_rt);
          cw__pw_msaa_create(&p->msaa_rt, scene);
        } else cw__pw_msaa_retarget(&p->msaa_rt, scene);
        p->msaa_color = scene->tex;
        scene_draw = &p->msaa_rt;
      } else if (s->msaa > 1 && (!wgpu || test_gl_msaa_on_webgpu)) { /* GL only: a WebGPU run has no GL context */
        GL_FEATURE("PostProcess msaa (the GL multisampled scene framebuffer)");
        unsigned m = msaa_fbo(p, w, h, sf, s->msaa);
        GL_FEATURE(NULL);
        if (m) {
          msaa_gl = (cw_rt){ .fbo = m, .w = w, .h = h, .format = sf };
          scene_draw = &msaa_gl;
        }
      }
    }
  }
  ctx.scene = scene;
  /* foliage (cartwheel/foliage.h): smooth cut-out edges by coverage when the scene pass is multisampled, hashed
   * otherwise; the hashed noise moves every frame under TAA */
  cw__foliage_frame(scene_draw && scene_draw != scene, taa, p->frame);
  p->scene_rt = scene, p->scene_draw_rt = scene_draw;
  p->prepass_ran = false;
  /* WebGPU MSAA: every render pass into the multisampled scene resolves its colour at its end. When the viewmodel
   * pass draws into it after the scene pass (the last opaque pass), the scene pass stores the samples only and the
   * viewmodel pass's resolve is the one (the same picture: the resolve after the last draw; three.c resolves first
   * for a transmission copy inside the scene). CW_TEST_WGPU_RESOLVE_TWICE=1 (native): both resolve, as before (the
   * gate's control). */
  p->msaa_keep = false;
  if (wgpu && scene_draw && scene_draw != scene && !post_run) {
    pass_rec *sc = NULL, *last = NULL;
    for (int i = 0; i < p->passes.count; i++) {
      pass_rec *r = p->passes.data[i];
      if (!r->active || r->d.phase != CW_PHASE_OPAQUE) continue;
      if (r->name == cw_intern("scene")) sc = r;
      last = r;
    }
    bool vm_last = last && last != sc && last->name == cw_intern("viewmodel");
    int opaque_n = 0;
    for (int i = 0; i < p->passes.count; i++) opaque_n += p->passes.data[i]->active && p->passes.data[i]->d.phase == CW_PHASE_OPAQUE;
#if defined(T3_EXTERNAL_TARGET_KEEP_MULTISAMPLED) && !CW_CART
    static int twice = -1;
    if (twice < 0) twice = getenv("CW_TEST_WGPU_RESOLVE_TWICE") && atoi(getenv("CW_TEST_WGPU_RESOLVE_TWICE")) ? 1 : 0;
    p->msaa_keep = sc && vm_last && opaque_n == 2 && !twice;
#elif defined(T3_EXTERNAL_TARGET_KEEP_MULTISAMPLED)
    p->msaa_keep = sc && vm_last && opaque_n == 2;
#endif
    p->msaa_resolves += (sc && !p->msaa_keep) + (vm_last ? 1 : 0);
  }

  /* the timers' results (once on they stay on, so every query issued is read) */
  if (cw__caps.timer_query && !wgpu && cw__gpu_timing_wanted() && p->run == 0 && p->frame % TIMER_EVERY == 0) { /* once (splitscreen runs once per view) */
    CW_PROFILE_SCOPE("pipeline.gpu_timers");
    GLint dj = 0;
    glGetIntegerv(GL_GPU_DISJOINT_EXT, &dj);
    for (int i = 0; i < p->passes.count; i++) timer_poll(p->passes.data[i], dj != 0, p->frame);
    for (int i = 0; i < p->steps.count; i++) timer_poll(p->steps.data[i], dj != 0, p->frame);
  }

  /* POST chain: the writers, so the last one can target the output */
  int last_writer = -1;
  for (int i = 0; i < p->passes.count; i++) {
    pass_rec *r = p->passes.data[i];
    if (r->active && r->d.phase == CW_PHASE_POST && (r->d.writes & CW_RES_COLOR) && post_in_run(p, r, mode)) last_writer = i;
  }
  /* a backdrop's picture: the scene target, or a target of its own when a depth-reading post pass writes colour */
  if (p->backdrop_run && scene) {
    if (last_writer >= 0) {
      const cw_rt *bo = cw_pipeline_rt_persistent(p, "backdrop.out", w, h, scene->format, NULL);
      if (bo) output = ctx.output = bo;
      else last_writer = -1;
    }
    p->bd_pic = last_writer >= 0 ? output : scene;
  }
  /* a depth copy for SKY / EFFECTS passes that sample depth */
  bool fx_reads_depth = false;
  for (int i = 0; i < p->passes.count; i++) {
    pass_rec *r = p->passes.data[i];
    if (r->active && (r->d.phase == CW_PHASE_SKY || r->d.phase == CW_PHASE_EFFECTS) && (r->d.reads & CW_RES_DEPTH))
      fx_reads_depth = true;
  }

  const cw_rt *chain = scene; /* POST: the current image */
  /* (GL too: each view's commands flushed as the view ends, so the GPU starts it while the CPU records the next) */
  bool view_major = mode == RUN_ARRAY && p->arr_n >= 2 && p->views_submit && !p->arr_refused;
  for (int ph = 0; ph < CW_PHASE_COUNT; ph++) {
    if (ph <= CW_PHASE_POST && !has_scene && !(post_run && ph == CW_PHASE_POST)) continue;
    /* WebGPU, one-pass split screen: every view's scene goes to the GPU before the views' effects are recorded, so
     * the GPU starts on it while the CPU records the rest (the per-view runs submitted each view's run as it ended) */
    /* WebGPU, one-pass split screen: each view's shadow-phase passes, depth prepass and opaque passes in turn, its
     * scene (an array camera of that one view, inside its rectangle of the one target) submitted as soon as it is
     * recorded, so the GPU draws a view while the CPU prepares the next. A host that waits for each frame otherwise saw
     * no GPU work until every view was prepared (the views drawn on their own each went to the GPU as they ended). */
    /* (GL: each view flushed to the GPU as it ends, glFlush: the GPU starts its work while the CPU records the next) */
    if (view_major && ph == CW_PHASE_SHADOW) {
      /* the scene's world matrices are brought up to date by the first view's renders; the later views draw them as
       * they stand (a walk of the whole scene a view saved: 3 ms of CPU a frame in a four-player race on GL) */
      bool keep_auto = ctx.scene3d && ctx.scene3d->auto_update;
      for (int k = 0; k < p->arr_n && !p->arr_refused; k++) {
        p->arr_part = k + 1;
        if (k == 1 && keep_auto) ctx.scene3d->auto_update = false;
        array_casters(&ctx, p, k);
        for (int ph2 = CW_PHASE_SHADOW; ph2 <= CW_PHASE_OPAQUE && !p->arr_refused; ph2++) {
          for (int i = 0; i < p->passes.count && !p->arr_refused; i++) {
            pass_rec *r = p->passes.data[i];
            if (!r->active || (int)r->d.phase != ph2 || !post_in_run(p, r, mode)) continue;
            ctx.src = ctx.dst = NULL;
            ctx.result = NULL;
            if (ph2 != CW_PHASE_SHADOW) bind_phase_target(&ctx, scene ? scene_draw : output);
            if (r->d.views == CW_PASS_VIEWS_EACH) {
              array_view(&ctx, p, k);
              run_pass(p, r, &ctx);
              array_view_end(&ctx, p);
            } else if (r->name == cw_intern("scene") || r->name == cw_intern("depth_prepass")) {
              if (r->name == cw_intern("scene")) views_stall();
              if (!p->arr_cam1) p->arr_cam1 = t3_array_camera_new();
              for (int c = 0; c < p->arr_cam1->count; c++) t3_release(p->arr_cam1->cameras[c]);
              p->arr_cam1->count = 0;
              t3_array_camera_add(p->arr_cam1, p->arr_cams[k], p->arr_vp[k][0], p->arr_vp[k][1], p->arr_vp[k][2], p->arr_vp[k][3]);
              ctx.camera = &p->arr_cam1->base;
              run_pass(p, r, &ctx);
              ctx.camera = p->arr_cams[0];
            } else if (k == 0) {
              run_pass(p, r, &ctx); /* (once for every view: the sky's cube) */
            }
            if (ph2 == CW_PHASE_PREPASS) p->prepass_ran = true;
            if (ph2 == CW_PHASE_OPAQUE) p->drew3d = true;
          }
        }
        /* this view to the GPU */
        if (wgpu) {
          cw__pw_flush();
          if (ctx.renderer) t3_renderer_wgpu_submit(ctx.renderer);
        } else {
          glFlush();
        }
      }
      if (keep_auto) ctx.scene3d->auto_update = true;
      array_casters(&ctx, p, 0);
      views_submits_dbg = p->arr_n;
      p->arr_part = 0;
      if (p->arr_refused) return; /* (a later view's pass could not: the views are drawn on their own; this target is not used) */
      ph = CW_PHASE_OPAQUE;
      continue;
    }
    if (wgpu && mode == RUN_ARRAY && ph == CW_PHASE_SKY && !p->arr_refused && !view_major) {
      cw__pw_flush();
      if (ctx.renderer) t3_renderer_wgpu_submit(ctx.renderer);
    }
    if ((mode == RUN_VIEW || mode == RUN_ARRAY) && ph >= CW_PHASE_OVERLAY) { /* a view draws no overlays: just its picture into its target */
      if (ph == CW_PHASE_OVERLAY && offscreen && last_writer < 0) {
        if (p->backdrop_run) {} /* (its picture is the scene target: bd_pic) */
        else if (p->view_post_split && (p->view_i >= 0 || mode == RUN_ARRAY) && scene->format == output->format) p->view_pic = scene; /* (no copy) */
        else cw__pass_copy(&ctx, scene->tex, output);
      }
      if (ph == CW_PHASE_OVERLAY && has_scene) {
        p->views_cpu_ms = (p->run ? p->views_cpu_ms : 0) + (float)(cw__render3d_now_ms() - t_start);
        cw__render3d_set_cpu_ms(t, p->views_cpu_ms);
      }
      continue;
    }
    if (offscreen && ph == CW_PHASE_SKY && wgpu) {
      /* leaving the opaque phase: MSAA colour is resolved already (each
       * render pass resolves); a multisampled depth (core WebGPU) resolves
       * into the scene's for the SKY / EFFECTS passes that depth-test against
       * it; the linear depth for passes that sample it */
      /* (the velocity pass depth-tests the meshes that moved against it too: velocity.c) */
      bool resolve_depth = scene_draw != scene && (fx_any || (velocity && velocity->active));
#if !CW_CART
      /* CW_TEST_WGPU_NO_DEPTH_RESOLVE=1: the resolve left out (a gate control: SKY / EFFECTS passes then
       * test against a scene depth nothing wrote) */
      if (getenv("CW_TEST_WGPU_NO_DEPTH_RESOLVE") && atoi(getenv("CW_TEST_WGPU_NO_DEPTH_RESOLVE"))) resolve_depth = false;
#endif
      int ts = step_begin(p, "resolve");
      if (resolve_depth) cw__pw_depth_resolve(scene_draw, scene);
      if (wgpu_depth || warm_depth) {
        const cw_rt *lin = cw_pipeline_rt_persistent(p, "scene.depth_linear", w, h, CW__RT_R32F, NULL);
        const float *e = ctx.camera->projection_matrix.e;
        float pz[4] = { e[10], e[14], e[11], e[15] };
        if (cw_gpu_reversed_depth()) {
          /* the z row minus the w row without the cancelling float difference */
          double n = ctx.camera->near, f = ctx.camera->far;
          bool plain = ctx.camera->base.type == T3_PERSPECTIVE_CAMERA && e[10] == (float)(-(f + n) / (f - n));
          pz[0] = plain ? (float)(-2 * n / (f - n)) : (float)((double)e[10] - e[11]);
        }
        if (lin && mode == RUN_ARRAY) { /* each view's rectangle through its own projection */
          for (int k = 0; k < p->arr_n; k++) {
            const float *ek = p->arr_cams[k]->projection_matrix.e;
            float pzk[4] = { ek[10], ek[14], ek[11], ek[15] };
            if (cw_gpu_reversed_depth()) {
              double n = p->arr_cams[k]->near, f = p->arr_cams[k]->far;
              bool plain = p->arr_cams[k]->base.type == T3_PERSPECTIVE_CAMERA && ek[10] == (float)(-(f + n) / (f - n));
              pzk[0] = plain ? (float)(-2 * n / (f - n)) : (float)((double)ek[10] - ek[11]);
            }
            array_view(&ctx, p, k);
            cw__pw_depth_copy(&ctx, scene_draw, lin, pzk);
          }
          array_view_end(&ctx, p);
          ctx.depth_tex = lin->tex;
        } else if (lin) {
          cw__pw_depth_copy(&ctx, scene_draw, lin, pz);
          ctx.depth_tex = lin->tex;
        }
      }
      step_end(ts);
    } else if (offscreen && ph == CW_PHASE_SKY) {
      /* leaving the opaque phase: resolve MSAA, copy depth for effects */
      int ts = step_begin(p, "resolve");
      if (scene_draw != scene) {
        unsigned scene_draw_fbo = scene_draw->fbo;
        glBindFramebuffer(GL_READ_FRAMEBUFFER, scene_draw_fbo);
        glBindFramebuffer(GL_DRAW_FRAMEBUFFER, scene->fbo);
        glBlitFramebuffer(0, 0, w, h, 0, 0, w, h, GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT, GL_NEAREST);
      }
      if (fx_reads_depth) {
        const cw_rt *dc = persistent_depth(p, "scene.depth_copy", w, h, CW_RT_R8);
        if (dc) {
          glBindFramebuffer(GL_READ_FRAMEBUFFER, scene->fbo);
          glBindFramebuffer(GL_DRAW_FRAMEBUFFER, dc->fbo);
          glBlitFramebuffer(0, 0, w, h, 0, 0, w, h, GL_DEPTH_BUFFER_BIT, GL_NEAREST);
          ctx.depth_tex = dc->depth_tex;
        }
      }
      step_end(ts);
      glBindFramebuffer(GL_FRAMEBUFFER, 0);
    }
    if (offscreen && ph == CW_PHASE_POST && !wgpu) ctx.depth_tex = scene->depth_tex;
    if (offscreen && ph == CW_PHASE_OVERLAY && last_writer < 0) cw__pass_copy(&ctx, scene->tex, output);
    /* WebGPU: the frame's passes so far go to the GPU before the overlays,
     * whose 2D renderer submits its own command buffers */
    if (wgpu && ph == CW_PHASE_OVERLAY) {
      cw__pw_flush();
      if (ctx.renderer) t3_renderer_wgpu_submit(ctx.renderer);
    }
    /* the scene drew at a share of the output (the Resolution setting,
     * cw_quality_scene_scale, pixel_scale with crisp UI): the overlays draw
     * at the output's full size. The scene is scaled up into the output (or
     * the picture's UI target) now, as the first 2D window pass would do it,
     * so every overlay pass, raw pass code included (cw_pass_bind_target(ctx,
     * NULL), cw_pass_fullscreen), sees the full-size output in ctx->output,
     * ctx->width and ctx->height. A custom output (cw_pipeline_set_output)
     * keeps its own size. */
    if (ph == CW_PHASE_OVERLAY && output == &p->screen && cw__pixel_target() == 1 && !overlay_scaled_test()) {
      cw__pixel_ui_begin();
      if (cw__pixel_target() != 1) {
        int ow = 0, oh = 0;
        cw_drawable_size(&ow, &oh);
        p->screen.fbo = cw__gl_window_fbo;
        p->screen.w = ow;
        p->screen.h = oh;
        ctx.width = ow;
        ctx.height = oh;
      }
    }
    if (ph == CW_PHASE_OVERLAY) {
      ctx.depth_tex = 0;
      if (has_scene) cw__render3d_set_cpu_ms(t, (float)(cw__render3d_now_ms() - t_start));
    }
    if (ph == CW_PHASE_OVERLAY && !(layers & CW_LAYER_WORLD)) {
      if (wgpu) cw_gpu_clear_window(0, 0, 0, 0);
      else {
      glBindFramebuffer(GL_FRAMEBUFFER, output->fbo);
      glViewport(0, 0, output->w, output->h);
      glDisable(GL_SCISSOR_TEST);
      glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
      glClearColor(0, 0, 0, 0);
      glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT | GL_STENCIL_BUFFER_BIT);
      }
    }
    /* the post prewarm's gi part (put on the lit materials by a prewarm run of the gi pass): the scene drawn with it once,
     * everything shown, into targets of its own; then taken off, so the frame's scene is drawn as it is without it */
    if (post_warm && ph == CW_PHASE_PREPASS && cw__gi_tagged(t) > gi_tagged0) {
      cw__scene_warm_draw(&ctx, NULL, false);
      cw__gi_untag_from(t, gi_tagged0);
    }
    /* the scene prewarm: everything in the scene drawn once as it is (hidden objects, instanced meshes of no instances),
     * into those targets. CW_TEST_NO_SCENE_PREWARM=1 leaves it out (a gate's control; getenv: a test's first frame) */
    if (scene_warm && ph == CW_PHASE_PREPASS) {
      const char *ns = getenv("CW_TEST_NO_SCENE_PREWARM");
      if (!(ns && atoi(ns))) cw__scene_warm_draw(&ctx, NULL, false), p->scene_prewarms++;
      p->scene_warm_armed = false;
    }
    if (!offscreen && (ph == CW_PHASE_PREPASS || ph == CW_PHASE_SKY || ph == CW_PHASE_EFFECTS || ph == CW_PHASE_POST))
      continue;
    for (int i = 0; i < p->passes.count; i++) {
      pass_rec *r = p->passes.data[i];
      if (!(r->active || r->warm_only) || (int)r->d.phase != ph || !post_in_run(p, r, mode)) continue;
      if (r->warm_only) { /* the post prewarm's run of a pass the frame has off: its picture into a target of its own */
        ctx.src = ph == CW_PHASE_POST ? chain : NULL;
        ctx.dst = ctx.result = NULL;
        ctx.settings = &warm_s;
        if (ph == CW_PHASE_POST && (r->d.writes & CW_RES_COLOR)) {
          ctx.dst = cw_pass_rt(&ctx, w, h, r->d.ldr_output ? CW_RT_RGBA8 : chain->format, false);
          if (ctx.dst) bind_phase_target(&ctx, ctx.dst);
          for (int k = 0; ctx.dst && k < p->pool.count; k++)
            if (&p->pool.data[k]->rt == ctx.dst) p->pool.data[k]->keep = true;
        }
        if (ph == CW_PHASE_SHADOW || ctx.dst || !(r->d.writes & CW_RES_COLOR)) {
          CW_PROFILE_BEGIN("pipeline.post_prewarm");
          /* a shadow-phase pass (the gi pass's G-buffers) draws every object of the scene: what is hidden now too */
          cw__show_keep keep = { 0 };
          if (ph == CW_PHASE_SHADOW) cw__scene_show_all(ctx.scene3d, &keep);
          if (mode == RUN_ARRAY && r->d.views == CW_PASS_VIEWS_EACH) {
            for (int k = 0; k < p->arr_n; k++) {
              if (ph == CW_PHASE_SHADOW) array_casters(&ctx, p, k);
              array_view(&ctx, p, k);
              run_pass(p, r, &ctx);
            }
            if (ph == CW_PHASE_SHADOW) array_casters(&ctx, p, 0);
            array_view_end(&ctx, p);
          } else run_pass(p, r, &ctx);
          cw__scene_show_restore(&keep);
          CW_PROFILE_END();
          p->post_warm_ran = true;
        }
        if (ctx.dst) cw_pass_rt_release(&ctx, ctx.dst);
        ctx.settings = s;
        ctx.src = ctx.dst = ctx.result = NULL;
        continue;
      }
      if (ph == CW_PHASE_OVERLAY) { /* layers: app = UI (HUDs), order >= 500 = DEBUG draw */
        if (r->d.order >= 500 ? !(layers & CW_LAYER_DEBUG) : !(layers & CW_LAYER_UI) && r->name == cw_intern("app")) continue;
      }
      ctx.src = ctx.dst = NULL;
      ctx.result = NULL;
      switch (ph) {
      case CW_PHASE_PREPASS:
      case CW_PHASE_OPAQUE: bind_phase_target(&ctx, scene ? scene_draw : output); break;
      case CW_PHASE_SKY:
      case CW_PHASE_EFFECTS: bind_phase_target(&ctx, scene); break;
      case CW_PHASE_POST:
        ctx.src = chain;
        if (p->fuse_bloom && r->name == cw_intern("bloom")) break; /* the glow only: the tonemap pass composites it */
        if (r->d.writes & CW_RES_COLOR) {
          if (i == last_writer) ctx.dst = output;
          else ctx.dst = cw_pass_rt(&ctx, w, h, r->d.ldr_output ? CW_RT_RGBA8 : chain->format, false);
          bind_phase_target(&ctx, ctx.dst);
        }
        break;
      case CW_PHASE_OVERLAY:
      case CW_PHASE_PRESENT: bind_phase_target(&ctx, output); break;
      default: break;
      }
      if (mode == RUN_ARRAY && r->d.views == CW_PASS_VIEWS_EACH && ph < CW_PHASE_OVERLAY) {
        for (int k = 0; k < p->arr_n && !p->arr_refused; k++) {
          if (ph == CW_PHASE_SHADOW) array_casters(&ctx, p, k);
          array_view(&ctx, p, k);
          run_pass(p, r, &ctx);
        }
        if (ph == CW_PHASE_SHADOW) array_casters(&ctx, p, 0); /* (three.c's own shadow maps: view 0's, as drawn view by view) */
        array_view_end(&ctx, p);
        /* a pass found it cannot this frame (cw__pipeline_array_refuse): nothing is drawn yet, the views go one at a
         * time instead */
        if (p->arr_refused) {
          CW_ASSERT(ph == CW_PHASE_SHADOW, "a one-pass split screen is refused only before anything is drawn");
          return;
        }
      } else if (mode == RUN_ARRAY && ph < CW_PHASE_OVERLAY) {
        if (r->name == cw_intern("scene") || r->name == cw_intern("depth_prepass") || r->name == cw_intern("transparents"))
          ctx.camera = &p->arr_cam->base; /* every view in one render */
        if (r->name == cw_intern("scene")) for (int k = 0; k < p->arr_n; k++) views_stall();
        run_pass(p, r, &ctx);
        ctx.camera = p->arr_cams[0];
      } else run_pass(p, r, &ctx);
      if (ph == CW_PHASE_PREPASS) p->prepass_ran = true;
      /* any opaque pass drew the 3D picture (the overlays then keep it) */
      if (ph == CW_PHASE_OPAQUE) p->drew3d = true;
      if (ph == CW_PHASE_POST && (r->d.writes & CW_RES_COLOR) && ctx.dst) {
        const cw_rt *prev = chain;
        chain = ctx.result ? ctx.result : ctx.dst;
        /* the image the chain moved past goes back to the pool */
        if (prev != scene) cw_pass_rt_release(&ctx, prev);
        if (ctx.result && ctx.dst != output) cw_pass_rt_release(&ctx, ctx.dst);
        /* the last writer redirected (into a history buffer): copy it out */
        if (i == last_writer && ctx.result && ctx.result != output) cw__pass_copy(&ctx, ctx.result->tex, output);
      }
    }
  }

  /* the post prewarm done: once (the views of a split screen after the first draw as they always do); the gi part it
   * put on the scene's materials taken off again unless a pass the frame runs reads it (it stays on once SSR or SSGI
   * first runs, as it does without a prewarm) */
  if (post_warm) {
    pass_rec *gi = find_pass(p, "gi");
    if (!(gi && gi->active)) cw__gi_untag_from(t, gi_tagged0);
    p->post_warm_armed = false;
    if (p->post_warm_ran) p->post_prewarms++;
  }

  /* pooled targets idle for a while are freed */
  for (int i = 0; i < p->pool.count; i++) {
    pool_rt *e = p->pool.data[i];
    e->used = false;
    if (p->frame - e->last_frame > 120 && !(e->keep && p->post_warm_have)) {
      cw__rt_destroy(&e->rt);
      cw_free(e);
      cw_array_remove_swap(&p->pool, i);
      i--;
    }
  }

  /* the frame's stats after its last run (cw_pipeline_render: frame_stats) */
  p->run++;

  if (wgpu) {
    cw__pw_flush();
    /* three.c recorded on the frame encoder: its frame closes after the submit */
    if (ctx.renderer) t3_renderer_wgpu_submit(ctx.renderer);
  }

  /* the program states no table had (those draws were skipped): add them to
   * the app's program table (cmake/programs.cmake, tools/programs.mjs). The
   * gates fail on a non-empty list and print the lines. */
  static int32_t missing_count;
  static char missing_text[16384]; /* the states, one per line (a list cut short hid states from the checks that read it) */
  static bool missing_field;
  if (!missing_field) {
    missing_field = true;
    cw_debug_i32("cw.three.missing", &missing_count);
    cw_debug_bytes("cw.three.missing_states", missing_text, sizeof missing_text);
  }
  if (ctx.renderer) {
    const char *m = t3_renderer_generated_missing(ctx.renderer);
    if (!p->missing_states || strcmp(p->missing_states, m ? m : "") != 0) {
      cw_free(p->missing_states);
      p->missing_states = cw_strdup(m ? m : "", CW_MEM_RENDER);
      int32_t n = 0;
      for (const char *c = p->missing_states; *c; c++)
        if (*c == '\n' || !c[1]) n++;
      if (n) {
        cw_error("render pipeline: %d draw state(s) have no program, so those draws are NOT DRAWN (the errors above name "
                 "each node and material). A material, light count or shadow setup the app's program table was not "
                 "surveyed with needs its program; so does content built on one graphics preset and drawn on another "
                 "(a preset changed in play). Fix: run\n"
                 "    %s\n"
                 "(a game built against the SDK: node <sdk>/tools/programs.mjs %s --build build/native, or its programs "
                 "script; add --carts for the cart's own states), then rebuild and commit the programs/ manifest and its "
                 "generated C. Guide: docs/guides/rendering-3d.md, \"Program tables\". Missing:\n%s",
                 (int)n, cw__programs_fix_command(), cw__app_target_name(), p->missing_states);
        const char *strict = getenv("CW_PROGRAMS_STRICT");
        if (strict && *strict && atoi(strict)) { /* CI and authors: an incomplete table is an exit (4), not a blank screen */
          fprintf(stderr, "CW_PROGRAMS_STRICT: exiting, the program table is incomplete\n");
          exit(4);
        }
      }
      /* the debug fields and the banner: every tree's states, not the last tree's to change (a Backdrop's or a
       * SubViewport's world renders with a renderer of its own, and its skipped draws went unreported to the gates and
       * the cart survey as soon as another tree's list changed; CW_TEST_MISSING_LAST_TREE=1: as before, a control) */
      const char *last = cw_test_env("CW_TEST_MISSING_LAST_TREE");
      if (last && atoi(last)) {
        memset(missing_text, 0, sizeof missing_text);
        snprintf(missing_text, sizeof missing_text, "%s", p->missing_states);
        missing_count = n;
      } else {
        missing_count = missing_union(missing_text, sizeof missing_text, p->missing_states);
      }
      cw__missing_programs = missing_count;
    }
  }

  /* undo the jitter so game code sees the camera's real projection */
  if (taa) t3_camera_update_projection_matrix(ctx.camera);
  if (mode == RUN_OVERLAY) return; /* the views' history stands */
  if (mode == RUN_ARRAY) {
    for (int k = 0; k < p->arr_n && k < CW_MAX_VIEWS; k++) {
      array_view(&ctx, p, k);
      p->view_prev[k].vp = ctx.view_proj;
      p->view_prev[k].have = offscreen;
      p->view_prev[k].w = p->arr_rect[k][2];
      p->view_prev[k].h = p->arr_rect[k][3];
    }
    array_view_end(&ctx, p);
    p->have_prev = false;
    return;
  }
  if (mode == RUN_VIEW && p->view_i >= CW_MAX_VIEWS) return;
  if (mode == RUN_VIEW && p->view_i >= 0) {
    p->view_prev[p->view_i].vp = ctx.view_proj;
    p->view_prev[p->view_i].have = offscreen;
    p->view_prev[p->view_i].w = w;
    p->view_prev[p->view_i].h = h;
    p->have_prev = false; /* a single view afterwards starts its own history */
    return;
  }
  if (offscreen) {
    p->prev_view_proj = ctx.view_proj;
    p->have_prev = true;
    p->prev_w = w;
    p->prev_h = h;
  } else {
    p->have_prev = false;
  }
}

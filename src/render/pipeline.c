/* The render pipeline: per-tree pass graph, target pool, GPU timing, and the
 * frame (pipeline.h). The built-in passes live in pipeline_post.c. */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>

#include "cartwheel/debug.h"
#include "cartwheel/profile.h"
#include "cartwheel/platform.h"
#include "cartwheel/render3d.h"
#include "pipeline_internal.h"
#include "quality_internal.h"
#include "pipeline_wgpu.h"
#include "scene/render3d_internal.h"

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
 * before WebGPU fell back to single-sampled. Its control only. */
static bool test_gl_msaa_on_webgpu;
static void cmd_pipeline_test(int argc, const char **argv, cw_reply *r) {
  if (argc == 2 && !strcmp(argv[1], "gl_msaa_on_webgpu")) {
    test_gl_msaa_on_webgpu = true;
    cw_reply_raw(r, "{\"ok\":true}");
  } else cw_reply_error(r, "usage: pipeline_test gl_msaa_on_webgpu");
}

static void cmd_pipeline(int argc, const char **argv, cw_reply *r) {
  (void)argc;
  (void)argv;
  gpu_timing = 1;
  cw_tree *t = cw_current_tree();
  cw_pipeline *p = t ? cw_tree_slot(t, "pipeline") : NULL;
  if (!p) {
    cw_reply_error(r, "no pipeline on the current tree (it is made on the first 3D frame)");
    return;
  }
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
  {
    /* the states three.c's generated table lacked (a JSON string, one per line) */
    cw_reply_raw(r, ",\"missing_states\":\"");
    for (const char *c = p->missing_states ? p->missing_states : ""; *c; c++) {
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
  cw_tree_set_slot(t, "pipeline", p, pipeline_free);
  cw__pipeline_add_builtin_passes(p);
  static bool cmd_done;
  if (!cmd_done) {
    cmd_done = true;
    cw_debug_command("pipeline", "pipeline: render passes (phase, ran, GPU ms), HDR format, timer support, missing three.c states", cmd_pipeline);
    cw_debug_command("pipeline_test", "pipeline_test gl_msaa_on_webgpu: a test control: puts back a GL call on WebGPU runs that the dual-cart check must catch",
                     cmd_pipeline_test);
  }
  return p;
}

cw_post_settings *cw_pipeline_settings(cw_pipeline *p) { return &p->s; }

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
static void plan_fusion(cw_pipeline *p, const cw_post_settings *s, bool post) {
  p->fuse_fog = p->fuse_bloom = p->fuse_grade = p->fuse_lens = p->fuse_lens_tm = false;
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
  if (tm >= 0 && post_adjacent(p, tm, gr)) {
    p->fuse_grade = true;
    fuse_off(p, gr);
  }
  /* the lens reads only its own pixel unless chromatic aberration is on */
  if (ln >= 0 && !s->chromatic_aberration) {
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

const cw_rt *cw_pipeline_rt_persistent(cw_pipeline *p, const char *key, int w, int h, cw_rt_format f, bool *fresh) {
  const char *k = cw_intern(key);
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
  const char *k = cw_intern(key);
  for (int i = 0; i < p->persist.count; i++)
    if (p->persist.data[i]->key == k) return p->persist.data[i]->rt.fbo ? &p->persist.data[i]->rt : NULL;
  return NULL;
}

static const cw_rt *persistent_depth(cw_pipeline *p, const char *key, int w, int h, cw_rt_format f) {
  const char *k = cw_intern(key);
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
  glRenderbufferStorageMultisample(GL_RENDERBUFFER, samples, f == CW_RT_RGBA16F ? GL_RGBA16F : GL_RGBA8, w, h);
  glBindRenderbuffer(GL_RENDERBUFFER, p->msaa_depth);
  glRenderbufferStorageMultisample(GL_RENDERBUFFER, samples, GL_DEPTH_COMPONENT24, w, h);
  glBindRenderbuffer(GL_RENDERBUFFER, 0);
  glBindFramebuffer(GL_FRAMEBUFFER, p->msaa_fbo);
  glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_RENDERBUFFER, p->msaa_color);
  glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_RENDERBUFFER, p->msaa_depth);
  if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) {
    cw_error("render pipeline: %dx MSAA on a %s target is not supported here; the scene renders without MSAA",
             samples, f == CW_RT_RGBA16F ? "RGBA16F" : "RGBA8");
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
      if (CW_PROFILE_ON()) cw__profile_gpu(r->name, r->q_eframe[best], (float)ns / 1e6f);
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

static int timer_begin(pass_rec *r, uint64_t frame) {
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

static float halton(int i, int b) {
  float f = 1, r = 0;
  while (i > 0) {
    f /= (float)b;
    r += f * (float)(i % b);
    i /= b;
  }
  return r;
}

static void run_pass(cw_pipeline *p, pass_rec *r, cw_pass_ctx *ctx) {
  bool wgpu = cw_gpu_backend() == CW_GPU_WEBGPU;
  cw__pass_state_reset();
  if (CW_PROFILE_ON()) cw_profile_begin(cw__profile_name2("pass ", r->name));
  int slot = timer_begin(r, p->frame);
  if (wgpu) cw__pw_timing_pass(p, r->name);
  ctx->user = r->d.user;
  double t0 = cw__render3d_now_ms();
  GL_FEATURE(r->name);
  r->d.execute(ctx, r->d.user);
  GL_FEATURE(NULL);
  r->frame_cpu += (float)(cw__render3d_now_ms() - t0);
  if (wgpu) cw__pw_timing_pass(p, NULL);
  timer_end(slot);
  r->stat.ran = true;
  CW_PROFILE_END();
}

/* bind the target a phase draws into */
static void bind_phase_target(cw_pass_ctx *ctx, const cw_rt *rt) {
  ctx->pipeline->bound_fbo = rt->fbo;
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
  return skip && !strcmp(skip, name);
#endif
}

/* what one run of the passes draws: everything (one camera fills the
 * output), one view's 3D picture (no overlays), or only the overlays over
 * the views already laid out */
enum { RUN_FULL, RUN_VIEW, RUN_OVERLAY };
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
  if (!p->view_post_split || r->d.phase != CW_PHASE_POST) return true;
  bool per_view = (r->d.reads & (CW_RES_DEPTH | CW_RES_VELOCITY)) != 0;
  return mode == RUN_VIEW ? per_view : !per_view;
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
  cw_pass_fullscreen();
}

void cw_pipeline_render(cw_tree *t, int w, int h, double alpha) {
  CW_PROFILE_SCOPE("pipeline.render");
  cw_pipeline *p = cw_pipeline_of(t);
  bool wgpu = cw_gpu_backend() == CW_GPU_WEBGPU;
  cw__pipeline_probe_caps();
  if (wgpu) {
    cw__pw_frame_begin();
    cw__pw_timing_poll();
  }
  if (p->order_dirty) {
    qsort(p->passes.data, (size_t)p->passes.count, sizeof *p->passes.data, pass_cmp);
    p->order_dirty = false;
  }
  p->run = 0;
  cw__render3d_frame_begin(t); /* the frame's draw counts start over */
  if (p->nviews < 2) {
    render_run(t, p, w, h, alpha, RUN_FULL);
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
  cw_rt_format vf = p->view_post_split && p->s.hdr && cw__caps.float_render ? CW_RT_RGBA16F : CW_RT_RGBA8;
  if (wgpu) cw__pw_timing_hold(true);
  const cw_rt *rts[CW_MAX_VIEWS];
  int n = 0;
  for (int i = 0; i < p->nviews; i++) {
    const cw_view *v = &p->views[i];
    int vw = CW_MAX(1, (int)(v->w * (float)out_copy.w + 0.5f)), vh = CW_MAX(1, (int)(v->h * (float)out_copy.h + 0.5f));
    char key[16];
    snprintf(key, sizeof key, "view%d", i);
    const cw_rt *rt = cw_pipeline_rt_persistent(p, key, vw, vh, vf, NULL);
    if (!rt || !cw_node_valid(v->camera)) continue;
    p->output_user = rt;
    cw_render3d_set_camera(t, v->camera);
    if (p->view_hook) p->view_hook(i, p->view_hook_user);
    cw__render3d_keep_shadows(t, n > 0); /* the shadow maps once a frame: the first view draws them */
    p->view_i = i;
    p->view_pic = NULL;
    render_run(t, p, vw, vh, alpha, RUN_VIEW);
    cw__visibility_end_view(t);
    rts[n++] = p->view_pic ? p->view_pic : rt;
  }
  p->view_i = -1;
  cw__render3d_keep_shadows(t, false);
  cw_render3d_set_camera(t, (cw_node){ 0 });
  p->output_user = keep_out;
  p->s.taa = keep_taa;
  /* shared post: the views laid out into one HDR picture, the chain's input */
  const cw_rt *laid = p->view_post_split && n ? cw_pipeline_rt_persistent(p, "views.post", out_copy.w, out_copy.h, vf, NULL) : NULL;
  if (n) views_composite(p, t, laid ? laid : &out_copy, rts, n);
  p->post_src = laid;
  /* the overlays once, over the whole output (the HUD reads cw_pipeline_views);
   * WebGPU reads the frame's pass timestamps back after it, not per view */
  if (wgpu) cw__pw_timing_hold(false);
  render_run(t, p, w, h, alpha, RUN_OVERLAY);
  p->post_src = NULL;
  p->view_post_split = false;
  frame_stats(p);
  p->frame++;
}

static void render_run(cw_tree *t, cw_pipeline *p, int w, int h, double alpha, int mode) {
  double t_start = cw__render3d_now_ms(); /* cw_render3d_stats.cpu_ms: up to the overlay phase */
  bool wgpu = cw_gpu_backend() == CW_GPU_WEBGPU;
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
  ctx.camera = mode == RUN_OVERLAY ? NULL : cw_render3d_prepare(t, w, h);
  CW_PROFILE_END();
  if (ctx.camera) {
    ctx.scene3d = cw_render3d_scene(t);
    ctx.camera_node = cw_render3d_camera(t);
    ctx.renderer = cw_render3d_renderer(t);
  }
  ctx.hdr = s->hdr && cw__caps.float_render;

  /* which passes run: settings-driven ones decide in active() */
  uint32_t reads = 0;
  bool post_or_fx = false, any_prepass = false, wgpu_opaque = false;
  pass_rec *velocity = NULL;
  for (int i = 0; i < p->passes.count; i++) {
    pass_rec *r = p->passes.data[i];
    if (p->run == 0) { /* a frame's first run: its stats start over (the views' runs add up) */
      r->stat.ran = r->stat.fused = false;
      r->frame_cpu = 0;
    }
    if (r->name == cw_intern("velocity")) {
      velocity = r;
      continue;
    }
    r->active = r->enabled && !test_skipped(r->name) && (!r->d.active || r->d.active(&ctx, r->d.user));
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
  if (velocity) velocity->active = velocity->enabled && !test_skipped(velocity->name) && (reads & CW_RES_VELOCITY) != 0;
  /* screenshot by layer (cartwheel/debug.h): without WORLD no 3D phase runs */
  uint32_t layers = cw_render_layers();
  bool has_scene = ctx.camera && (ctx.renderer != NULL || (wgpu && wgpu_opaque)) && (layers & CW_LAYER_WORLD);
  p->drew3d = mode == RUN_OVERLAY; /* the views' pictures are already there: the overlays keep them */
  /* WebGPU always renders the scene offscreen: the window texture has no
   * depth */
  bool offscreen = has_scene && (wgpu || post_or_fx || s->hdr || s->msaa > 1 || any_prepass || (reads & (CW_RES_DEPTH | CW_RES_VELOCITY)));
  /* splitscreen's shared post: the overlay run starts from the laid-out views */
  bool post_run = mode == RUN_OVERLAY && p->post_src;
  if (post_run) offscreen = true;
  ctx.offscreen = offscreen;
  plan_fusion(p, s, offscreen);

  /* TAA jitter + camera matrices (offscreen only: the direct path leaves the
   * camera exactly as three.c's own path does) */
  bool taa = offscreen && s->taa && !post_run;
  if (offscreen && !post_run) {
    mat4_from_camera(&ctx);
    if (!p->have_prev || p->prev_w != w || p->prev_h != h) ctx.prev_view_proj = ctx.view_proj;
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

  /* three.c: scene-linear output into the HDR target (r186 programs, physical lights, colour
   * management); exposure, the tone curve and the sRGB transfer are the tonemap pass's */
  if (ctx.renderer) {
    t3_renderer_set_tone_mapping(ctx.renderer, T3_NO_TONE_MAPPING, 1);
    t3_renderer_set_output_color_space(ctx.renderer, T3_LINEAR_SRGB_COLOR_SPACE);
  }

  /* WebGPU: passes sample an R32F linear depth copied after the opaque phase */
  bool wgpu_depth = false;
  for (int i = 0; wgpu && i < p->passes.count; i++) {
    pass_rec *r = p->passes.data[i];
    if (r->active && r->d.phase >= CW_PHASE_SKY && r->d.phase <= CW_PHASE_POST && (r->d.reads & (CW_RES_DEPTH | CW_RES_VELOCITY)))
      wgpu_depth = true;
  }
  bool fx_any = false;
  for (int i = 0; i < p->passes.count; i++) {
    pass_rec *r = p->passes.data[i];
    if (r->active && (r->d.phase == CW_PHASE_SKY || r->d.phase == CW_PHASE_EFFECTS)) fx_any = true;
  }
  /* the scene target */
  const cw_rt *scene = post_run ? p->post_src : NULL, *scene_draw = NULL;
  cw_rt msaa_gl;
  if (offscreen && !post_run) {
    cw_rt_format sf = ctx.hdr ? CW_RT_RGBA16F : CW_RT_RGBA8;
    /* a shared post chain's views each keep their own scene target: it is
     * the view's picture, laid out as it is */
    char skey[24] = "scene";
    if (mode == RUN_VIEW && p->view_post_split && p->view_i >= 0) snprintf(skey, sizeof skey, "scene.v%d", p->view_i);
    scene = persistent_depth(p, skey, w, h, sf);
    if (!scene) offscreen = ctx.offscreen = false;
    else {
      scene_draw = scene;
      static bool msaa_warned;
      /* compatibility mode (the tier's floor): RGBA16Float cannot be
       * multisampled, and a multisampled depth cannot be read back (no
       * textureLoad on depth, no sampling of multisampled textures), so 4x
       * MSAA serves RGBA8 scenes that no SKY / EFFECTS / depth-reading pass
       * follows; anything else renders single-sampled (logged once) */
      bool msaa_ok = sf == CW_RT_RGBA8 && !wgpu_depth && !fx_any;
      if (s->msaa > 1 && wgpu && !msaa_ok && !msaa_warned) {
        msaa_warned = true;
        cw_warn("render pipeline: MSAA on WebGPU (compatibility mode) needs an RGBA8 scene (hdr off) and no SKY / EFFECTS / "
                "depth-reading pass; this frame renders without MSAA");
      }
      if (s->msaa > 1 && wgpu && msaa_ok) {
        /* 4x (compatibility mode's only count), resolved by each render pass */
        if (!p->msaa_rt.fbo || p->msaa_rt.w != w || p->msaa_rt.h != h || p->msaa_rt.format != sf || p->msaa_color != scene->tex) {
          if (p->msaa_rt.fbo) cw__rt_destroy(&p->msaa_rt);
          cw__pw_msaa_create(&p->msaa_rt, scene);
          p->msaa_color = scene->tex;
        }
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
  p->prepass_ran = false;

  /* the timers' results (once on they stay on, so every query issued is read) */
  if (cw__caps.timer_query && !wgpu && cw__gpu_timing_wanted() && p->run == 0 && p->frame % TIMER_EVERY == 0) { /* once (splitscreen runs once per view) */
    CW_PROFILE_SCOPE("pipeline.gpu_timers");
    GLint dj = 0;
    glGetIntegerv(GL_GPU_DISJOINT_EXT, &dj);
    for (int i = 0; i < p->passes.count; i++) timer_poll(p->passes.data[i], dj != 0, p->frame);
  }

  /* POST chain: the writers, so the last one can target the output */
  int last_writer = -1;
  for (int i = 0; i < p->passes.count; i++) {
    pass_rec *r = p->passes.data[i];
    if (r->active && r->d.phase == CW_PHASE_POST && (r->d.writes & CW_RES_COLOR) && post_in_run(p, r, mode)) last_writer = i;
  }
  /* a depth copy for SKY / EFFECTS passes that sample depth */
  bool fx_reads_depth = false;
  for (int i = 0; i < p->passes.count; i++) {
    pass_rec *r = p->passes.data[i];
    if (r->active && (r->d.phase == CW_PHASE_SKY || r->d.phase == CW_PHASE_EFFECTS) && (r->d.reads & CW_RES_DEPTH))
      fx_reads_depth = true;
  }

  const cw_rt *chain = scene; /* POST: the current image */
  for (int ph = 0; ph < CW_PHASE_COUNT; ph++) {
    if (ph <= CW_PHASE_POST && !has_scene && !(post_run && ph == CW_PHASE_POST)) continue;
    if (mode == RUN_VIEW && ph >= CW_PHASE_OVERLAY) { /* a view draws no overlays: just its picture into its target */
      if (ph == CW_PHASE_OVERLAY && offscreen && last_writer < 0) {
        if (p->view_post_split && p->view_i >= 0 && scene->format == output->format) p->view_pic = scene; /* (no copy) */
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
       * render pass resolves); the linear depth for passes that sample it */
      if (wgpu_depth) {
        const cw_rt *lin = cw_pipeline_rt_persistent(p, "scene.depth_linear", w, h, CW__RT_R32F, NULL);
        const float *e = ctx.camera->projection_matrix.e;
        float pz[4] = { e[10], e[14], e[11], e[15] };
        if (lin) {
          cw__pw_depth_copy(&ctx, scene_draw, lin, pz);
          ctx.depth_tex = lin->tex;
        }
      }
    } else if (offscreen && ph == CW_PHASE_SKY) {
      /* leaving the opaque phase: resolve MSAA, copy depth for effects */
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
    if (!offscreen && (ph == CW_PHASE_PREPASS || ph == CW_PHASE_SKY || ph == CW_PHASE_EFFECTS || ph == CW_PHASE_POST))
      continue;
    for (int i = 0; i < p->passes.count; i++) {
      pass_rec *r = p->passes.data[i];
      if (!r->active || (int)r->d.phase != ph || !post_in_run(p, r, mode)) continue;
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
      run_pass(p, r, &ctx);
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

  /* pooled targets idle for a while are freed */
  for (int i = 0; i < p->pool.count; i++) {
    pool_rt *e = p->pool.data[i];
    e->used = false;
    if (p->frame - e->last_frame > 120) {
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
  static char missing_text[1024]; /* the states, one per line (truncated) */
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
      cw__missing_programs = n;
      if (n) {
        cw_error("render pipeline: %d draw state(s) are in none of the app's program tables, so those draws are NOT DRAWN "
                 "(a new material, light count or shadow combination needs its program). Fix: from the engine checkout run\n"
                 "    %s\nthen commit the changed programs/ manifest and generated C and rebuild (a project outside the tree: "
                 "node tools/programs.mjs --projects <dir>). Missing:\n%s", (int)n, cw__programs_fix_command(), p->missing_states);
        const char *strict = getenv("CW_PROGRAMS_STRICT");
        if (strict && *strict && atoi(strict)) { /* CI and authors: an incomplete table is an exit (4), not a blank screen */
          fprintf(stderr, "CW_PROGRAMS_STRICT: exiting, the program table is incomplete\n");
          exit(4);
        }
      }
      missing_count = n;
      memset(missing_text, 0, sizeof missing_text);
      snprintf(missing_text, sizeof missing_text, "%s", p->missing_states);
    }
  }

  /* undo the jitter so game code sees the camera's real projection */
  if (taa) t3_camera_update_projection_matrix(ctx.camera);
  if (mode == RUN_OVERLAY) return; /* the views' history stands */
  if (offscreen) {
    p->prev_view_proj = ctx.view_proj;
    p->have_prev = true;
    p->prev_w = w;
    p->prev_h = h;
  } else {
    p->have_prev = false;
  }
}

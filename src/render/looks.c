/* Looks (cartwheel/look.h): the "looks" POST pass, which restyles the scene with up to two looks and blends them.
 *
 * Every look reads the chain's image (scene-linear HDR, before bloom) times the frame's exposure and writes its
 * colour divided by it again. All grids are anchored to the view's top left pixel (tl: view pixels from the top left,
 * pixel centres at .5). Intermediate targets of a look (ASCII cells, half block pixels, dither states) are the view's
 * own size and drawn with the one-pass split screen's scissor off; the final draws go into the full target inside the
 * view's rectangle. GLSL and WGSL are the same programs line for line. */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cartwheel/debug.h"
#include "cartwheel/gpu.h"
#include "cartwheel/look.h"
#include "cartwheel/math.h"
#include "cartwheel/platform.h"
#include "cartwheel/profile.h"
#include "cartwheel/vfs.h"
#include "look_blue_noise.h"
#include "look_internal.h"
#include "pipeline_internal.h"

#define MAX_LOOKS 64
#define NGLYPH 95 /* printable ASCII, space first */
#define FIRST_CP 32
static const char DEF_RAMP[] = " .:-=+*#%@";
static const char DEF_GLYPHS[] = ".,:;'`\"-_=+*^~|/\\()[]{}<>!?lLJT7YVvo";

static bool wg(void) { return cw_gpu_backend() == CW_GPU_WEBGPU; }
uint16_t lm_float_to_half(float f); /* src/render/lightmap_bvh.c */

/* ── looks ──────────────────────────────────────────────────────────── */

typedef struct look_atlas {
  int cw, ch;
  unsigned tex;
  uint64_t used;
} look_atlas;

typedef struct look {
  uint32_t gen;
  bool live;
  cw_look_desc d;
  char *glyphs, *ramp, *font, *glsl, *wgsl;
  float pal[CW_LOOK_MAX_PALETTE * 3];
  int ver;
  /* ASCII: the font, the glyph shape vectors (+ ramp and allowed set), atlases at the cell sizes in use */
  int ascii_ver;
  cw__look_font *fnt;
  float aspect;
  unsigned gv_tex;
  int ramp_n;
  look_atlas atlas[4];
  uint64_t atlas_clock;
  /* HALFBLOCK: the palette as a texture (display steps in rgb, then linear) */
  unsigned pal_tex;
  int pal_ver;
  /* CUSTOM: its program at each stage */
  unsigned prog[2];
  int prog_ver[2];
} look;
static look LOOKS[MAX_LOOKS];

static look *get(cw_look h) {
  uint32_t i = CW_HANDLE_INDEX(h), g = CW_HANDLE_GEN(h);
  if (CW_HANDLE_IS_NULL(h) || i == 0 || i >= MAX_LOOKS) return NULL;
  look *l = &LOOKS[i];
  return l->live && l->gen == g ? l : NULL;
}
/* a look that draws something (NULL: the plain scene) */
static look *drawable(cw_look h) {
  look *l = get(h);
  return l && l->d.kind != CW_LOOK_NONE ? l : NULL;
}

cw_look_desc cw_look_default(cw_look_kind kind) {
  cw_look_desc d = { .kind = kind };
  d.ascii = (cw_look_ascii){ .rows = 54, .neon = 1.6f, .saturation = 1.8f, .tint = { 0.2f, 1.0f, 0.45f }, .glow = 0.5f,
                             .glow_from = 0.5f, .hold = 1.35f, .exposure = 1, .floor = 0.05f, .contrast = 2, .edges = 1.5f,
                             .flat = 0.45f, .ramp_gain = 1, .flat_bright = 0.75f, .dim_bright = 0.3f, .bright_floor = 0.35f,
                             .peak = 0.5f };
  d.halfblock = (cw_look_halfblock){ .rows = 54 };
  d.dither = (cw_look_dither){ .pixel = 2, .light = { 1, 1, 1 }, .pattern = CW_DITHER_BAYER, .contrast = 1, .sweeps = 2,
                               .hue_from = 0.22f, .hue_to = 0.5f, .grey_gain = 1, .keep_from = 0.6f, .keep_to = 0.95f,
                               .sharpen_radius = 2, .outline_from = 0.02f, .outline_to = 0.08f, .gamma = 1 };
  return d;
}

static char *dup_or_null(const char *s) { return s ? cw_strdup(s, CW_MEM_RENDER) : NULL; }

static void look_release_gpu(look *l) {
  for (int i = 0; i < 4; i++) cw_pass_texture_free(l->atlas[i].tex), l->atlas[i] = (look_atlas){ 0 };
  cw_pass_texture_free(l->gv_tex), l->gv_tex = 0;
  cw_pass_texture_free(l->pal_tex), l->pal_tex = 0;
  cw__look_font_free(l->fnt), l->fnt = NULL;
  l->ascii_ver = l->pal_ver = l->prog_ver[0] = l->prog_ver[1] = 0;
}

static void look_free_strings(look *l) {
  cw_free(l->glyphs), cw_free(l->ramp), cw_free(l->font), cw_free(l->glsl), cw_free(l->wgsl);
  l->glyphs = l->ramp = l->font = l->glsl = l->wgsl = NULL;
}

static bool desc_ok(const cw_look_desc *d) {
  if (!d || d->kind < CW_LOOK_NONE || d->kind >= CW_LOOK_KIND_COUNT) {
    cw_error("cw_look: a desc of an unknown kind");
    return false;
  }
  if (d->kind == CW_LOOK_CUSTOM && !(wg() ? d->custom.wgsl : d->custom.glsl)) {
    cw_error("cw_look: a CUSTOM look needs its %s (custom.%s)", wg() ? "WGSL" : "GLSL", wg() ? "wgsl" : "glsl");
    return false;
  }
  return true;
}

static void look_store(look *l, const cw_look_desc *d) {
  bool font_changed = (d->kind == CW_LOOK_ASCII) &&
                      ((!l->font) != (!d->ascii.font) || (l->font && d->ascii.font && strcmp(l->font, d->ascii.font)));
  look_free_strings(l);
  l->d = *d;
  l->glyphs = dup_or_null(d->ascii.glyphs);
  l->ramp = dup_or_null(d->ascii.ramp);
  l->font = dup_or_null(d->ascii.font);
  l->glsl = dup_or_null(d->custom.glsl);
  l->wgsl = dup_or_null(d->custom.wgsl);
  l->d.ascii.glyphs = l->glyphs, l->d.ascii.ramp = l->ramp, l->d.ascii.font = l->font;
  l->d.custom.glsl = l->glsl, l->d.custom.wgsl = l->wgsl;
  int n = CW_CLAMP(d->halfblock.palette ? d->halfblock.palette_count : 0, 0, CW_LOOK_MAX_PALETTE);
  if (n) memcpy(l->pal, d->halfblock.palette, sizeof(float) * 3 * (size_t)n);
  l->d.halfblock.palette = n ? l->pal : NULL;
  l->d.halfblock.palette_count = n;
  if (font_changed && l->fnt) {
    cw__look_font_free(l->fnt), l->fnt = NULL;
    for (int i = 0; i < 4; i++) cw_pass_texture_free(l->atlas[i].tex), l->atlas[i] = (look_atlas){ 0 };
  }
  l->ver++;
}

cw_look cw_look_new(const cw_look_desc *d) {
  if (!desc_ok(d)) return (cw_look){ 0 };
  for (int i = 1; i < MAX_LOOKS; i++) {
    look *l = &LOOKS[i];
    if (l->live) continue;
    uint32_t gen = l->gen + 1;
    if (gen == 0) gen = 1;
    memset(l, 0, sizeof *l);
    l->gen = gen;
    l->live = true;
    look_store(l, d);
    return CW_HANDLE_MAKE(cw_look, i, gen);
  }
  cw_error("cw_look_new: %d looks live already (free some with cw_look_free)", MAX_LOOKS - 1);
  return (cw_look){ 0 };
}

bool cw_look_set(cw_look h, const cw_look_desc *d) {
  look *l = get(h);
  if (!l || !desc_ok(d)) return false;
  look_store(l, d);
  return true;
}

bool cw_look_get(cw_look h, cw_look_desc *out) {
  look *l = get(h);
  if (!l || !out) return false;
  *out = l->d;
  return true;
}

void cw_look_free(cw_look h) {
  look *l = get(h);
  if (!l) return;
  look_release_gpu(l);
  look_free_strings(l);
  uint32_t gen = l->gen;
  memset(l, 0, sizeof *l);
  l->gen = gen;
}

/* ── per pipeline state ──────────────────────────────────────────────── */

typedef struct looks_state {
  cw_pipeline *p;
  cw_look a, b;
  float t;
  cw_look_mix how;
  cw_look_box box[CW_LOOK_MAX_BOXES];
  int nbox;
  /* glyph history: the frame a (look, view slot) last drew */
  struct { uint32_t look, slot; uint64_t frame; } seen[32];
  int nseen;
  /* brightness measurement */
  bool measure;
  float lum[2];
  bool have_lum;
  uint32_t ticket[2];
  bool pend[2];       /* look.lum<i> holds a reduction not read yet */
  uint8_t drawn;      /* the looks drawn this frame (bit i) */
  /* prewarms (cw_look_prewarm): looks drawn once at the next frame, each at the stages it can be drawn at (bit 0
   * SCENE, bit 1 DISPLAY), into the pass's own target before its real run writes it */
  struct { cw_look l; uint8_t stages; } warm[MAX_LOOKS];
  int nwarm;
  int32_t prewarms;
} looks_state;

static looks_state *STATES[16];
static int NSTATES;

static void state_destroy(void *u) {
  looks_state *s = u;
  for (int i = 0; i < NSTATES; i++)
    if (STATES[i] == s) STATES[i] = STATES[--NSTATES];
  for (int i = 0; i < 2; i++)
    if (s->ticket[i]) cw_gpu_read_drop(s->ticket[i]);
  cw_free(s);
}

static looks_state *state_find(cw_pipeline *p) {
  for (int i = 0; i < NSTATES; i++)
    if (STATES[i]->p == p) return STATES[i];
  return NULL;
}

/* debug fields (the pipeline drawn last) */
static int32_t dbg_ran, dbg_cells, dbg_views, dbg_kind_a, dbg_kind_b, dbg_stage, dbg_tone, dbg_tone_small;
static float dbg_t;

static bool looks_active(const cw_pass_ctx *ctx, void *u);
static void looks_exec(cw_pass_ctx *ctx, void *u);
static bool looks_display_active(const cw_pass_ctx *ctx, void *u);
static void looks_display_exec(cw_pass_ctx *ctx, void *u);

static looks_state *state_of(cw_pipeline *p) {
  looks_state *s = state_find(p);
  if (s) return s;
  if (NSTATES >= (int)CW_ARRAY_LEN(STATES)) {
    cw_error("looks: more than %d pipelines draw looks", (int)CW_ARRAY_LEN(STATES));
    return NULL;
  }
  s = cw_calloc(1, sizeof *s, CW_MEM_RENDER);
  s->p = p;
  s->lum[0] = s->lum[1] = -1;
  STATES[NSTATES++] = s;
  static bool fields;
  if (!fields) {
    fields = true;
    cw_debug_i32("look.ran", &dbg_ran);
    cw_debug_i32("look.cells", &dbg_cells);
    cw_debug_i32("look.views", &dbg_views);
    cw_debug_i32("look.kind_a", &dbg_kind_a);
    cw_debug_i32("look.kind_b", &dbg_kind_b);
    cw_debug_f32("look.t", &dbg_t);
    cw_debug_i32("look.stage", &dbg_stage);
    cw_debug_i32("look.tone", &dbg_tone);
    cw_debug_i32("look.tone_small", &dbg_tone_small);
  }
  /* after automatic exposure measured the scene (290), before bloom (300) */
  cw_pipeline_add_pass(p, &(cw_pass_desc){ .name = "looks", .phase = CW_PHASE_POST, .order = 295,
                                           .reads = CW_RES_COLOR | CW_RES_DEPTH, .writes = CW_RES_COLOR,
                                           .active = looks_active, .execute = looks_exec, .destroy = state_destroy, .user = s,
                                           .webgpu = true, .views = CW_PASS_VIEWS_EACH });
  /* the DISPLAY stage: last, after the lens and the game's own POST passes (debug views at 10000 still show over it) */
  cw_pipeline_add_pass(p, &(cw_pass_desc){ .name = "looks_display", .phase = CW_PHASE_POST, .order = 9900,
                                           .reads = CW_RES_COLOR | CW_RES_DEPTH, .writes = CW_RES_COLOR,
                                           .active = looks_display_active, .execute = looks_display_exec, .user = s,
                                           .webgpu = true, .views = CW_PASS_VIEWS_EACH });
  return s;
}

void cw_pipeline_set_looks(cw_pipeline *p, cw_look a, cw_look b, float t, const cw_look_mix *how) {
  looks_state *s = state_find(p);
  if (!s && CW_HANDLE_IS_NULL(a) && CW_HANDLE_IS_NULL(b)) return; /* nothing was ever set: nothing to register */
  if (!s && !(s = state_of(p))) return;
  s->a = a, s->b = b;
  s->t = t != t ? 0 : CW_CLAMP(t, 0.0f, 1.0f);
  s->how = how ? *how : (cw_look_mix){ 0 };
}

void cw_pipeline_looks(cw_pipeline *p, cw_look *a, cw_look *b, float *t, cw_look_mix *how) {
  looks_state *s = state_find(p);
  if (a) *a = s ? s->a : (cw_look){ 0 };
  if (b) *b = s ? s->b : (cw_look){ 0 };
  if (t) *t = s ? s->t : 0;
  if (how) *how = s ? s->how : (cw_look_mix){ 0 };
}

void cw_pipeline_set_look_boxes(cw_pipeline *p, const cw_look_box *boxes, int n) {
  looks_state *s = state_find(p);
  if (!s && n <= 0) return;
  if (!s && !(s = state_of(p))) return;
  s->nbox = CW_CLAMP(n, 0, CW_LOOK_MAX_BOXES);
  if (s->nbox) memcpy(s->box, boxes, sizeof *boxes * (size_t)s->nbox);
}

void cw_pipeline_measure_looks(cw_pipeline *p, bool on) {
  looks_state *s = state_find(p);
  if (!s && !on) return;
  if (!s && !(s = state_of(p))) return;
  s->measure = on;
  if (!on) {
    s->have_lum = false;
    for (int i = 0; i < 2; i++) {
      if (s->ticket[i]) cw_gpu_read_drop(s->ticket[i]);
      s->ticket[i] = 0, s->pend[i] = false, s->lum[i] = -1;
    }
  }
}

bool cw_pipeline_look_luminance(cw_pipeline *p, float out[2]) {
  looks_state *s = state_find(p);
  if (!s || !s->have_lum) return false;
  out[0] = s->lum[0], out[1] = s->lum[1];
  return true;
}

/* what the frame draws: the first look (t < 1), the second (t > 0) */
static void plan(const looks_state *s, look **la, look **lb, bool *need_a, bool *need_b) {
  *la = drawable(s->a), *lb = drawable(s->b);
  if (s->a.id == s->b.id) *lb = *la; /* the same look twice: drawn once */
  *need_a = s->t < 1.0f, *need_b = s->t > 0.0f;
  if (*la == *lb && *need_a && *need_b) *need_b = false, *need_a = true; /* (a blend of a look with itself is that look) */
}

/* the stages the frame draws at (bit 0 SCENE, bit 1 DISPLAY): each look drawn at its own stage, only a look with
 * weight counts. A blend of looks of two stages (0 < t < 1) runs at both, each stage blending its own look with the
 * picture it gets by the pair's weight and mask (the other look the plain picture there), so the blend starts on
 * the first look's own picture and ends on the second's with nothing to jump to when it is over (a pair drawn at
 * one stage the whole way ended on the second look at the other stage and jumped there in a frame).
 * CW_TEST_LOOK_ONE_STAGE=1 (a gate's control): the old rule, the pair at DISPLAY when either look asks for it,
 * whatever the weight. */
static int test_int(const char *name);
static int pair_stages(const looks_state *s) {
  look *la, *lb;
  bool na, nb;
  plan(s, &la, &lb, &na, &nb);
  if (test_int("CW_TEST_LOOK_ONE_STAGE")) {
    const look *a = drawable(s->a), *b = drawable(s->b);
    bool d = (a && a->d.stage == CW_LOOK_STAGE_DISPLAY) || (b && b->d.stage == CW_LOOK_STAGE_DISPLAY);
    return ((la && (na || la == lb)) || (nb && lb)) ? (d ? 2 : 1) : 0;
  }
  int m = 0;
  if (la && (na || la == lb)) m |= la->d.stage == CW_LOOK_STAGE_DISPLAY ? 2 : 1;
  if (lb && nb && lb != la) m |= lb->d.stage == CW_LOOK_STAGE_DISPLAY ? 2 : 1;
  return m;
}
/* a prewarm waiting for this stage */
static bool warm_wants(const looks_state *s, int stage) {
  for (int i = 0; i < s->nwarm; i++)
    if (s->warm[i].stages & (1u << stage)) return true;
  return false;
}
static bool looks_active(const cw_pass_ctx *ctx, void *u) {
  (void)ctx;
  return (pair_stages(u) & 1) != 0 || warm_wants(u, 0);
}
static bool looks_display_active(const cw_pass_ctx *ctx, void *u) {
  /* split screen with the shared post chain: the views' pictures go through one chain after they are laid out, so
   * the DISPLAY stage runs once over them, after it (a pass reading the depth would run in each view's run, before
   * the chain); the views' depth is not there (no outlines, cw_look_depth reads nothing drawn) */
  const cw_pipeline *p = ctx->pipeline;
  bool shared = p->views_shared_post && p->nviews >= 2;
  cw__pipeline_set_pass_reads((cw_pipeline *)p, "looks_display", shared ? CW_RES_COLOR : CW_RES_COLOR | CW_RES_DEPTH);
  return (pair_stages(u) & 2) != 0 || warm_wants(u, 1);
}

/* ── shaders ─────────────────────────────────────────────────────────── */

/* the common part of every look program: the image (unit 0), the automatic exposure (unit 1), the view's rectangle
 * (u_rect: x, y from the bottom left of the target, w, h), u_ex (the settings' exposure, a pattern offset (the crawl
 * control), pixels per pixel of a 1080 line picture, cw_time), u_bn (box count, frame), the boxes (u_b0..15: x0, y0,
 * x1, y1 in view pixels from the top left) and their values (u_k0..3: dim 0..1, 2 = clean) */
#define BOXES_16(P, S) P "0" S P "1" S P "2" S P "3" S P "4" S P "5" S P "6" S P "7" S P "8" S P "9" S P "10" S P "11" S \
                       P "12" S P "13" S P "14" S P "15" S
static const char *G_COMMON =
    "uniform sampler2D u_src;\n"
    "uniform sampler2D u_auto;\n"
    "uniform vec4 u_rect; uniform vec4 u_ex; uniform vec4 u_bn;\n"
    BOXES_16("uniform vec4 u_b", ";\n")
    "uniform vec4 u_k0; uniform vec4 u_k1; uniform vec4 u_k2; uniform vec4 u_k3;\n"
    "out vec4 o;\n"
    "vec2 FC;\n";
static const char *G_FUNCS =
    "vec3 look_in(vec3 c); vec3 look_out(vec3 c); vec3 look_tsrc(vec2 tl); float look_tv(float y); vec4 look_tone4(vec3 c);\n"
    "float lum(vec3 c) { return dot(c, vec3(0.2126, 0.7152, 0.0722)); }\n"
    "float look_E() { return u_ex.x * exp2(dot(texelFetch(u_auto, ivec2(0), 0).rg, vec2(1.0, 1.0 / 1024.0))); }\n"
    "vec2 look_tl() { return vec2(FC.x - u_rect.x, u_rect.y + u_rect.w - FC.y); }\n"
    "vec2 look_global(vec2 tl) { return vec2(u_rect.x + tl.x, u_rect.y + u_rect.w - tl.y); }\n"
    "vec3 look_src(vec2 tl) {\n"
    "  vec2 g = clamp(look_global(tl), u_rect.xy + 0.5, u_rect.xy + u_rect.zw - 0.5);\n"
    "  return look_in(texture(u_src, g / vec2(textureSize(u_src, 0))).rgb);\n"
    "}\n"
    "vec3 look_src_raw(vec2 tl) {\n"
    "  ivec2 q = clamp(ivec2(floor(look_global(tl))), ivec2(u_rect.xy), ivec2(u_rect.xy + u_rect.zw) - 1);\n"
    "  return texelFetch(u_src, q, 0).rgb;\n"
    "}\n"
    "vec3 look_src_px(vec2 tl) { return look_in(look_src_raw(tl)); }\n"
    "float look_box(vec2 lo, vec2 hi) {\n"
    "  vec4 b[16] = vec4[16](u_b0, u_b1, u_b2, u_b3, u_b4, u_b5, u_b6, u_b7, u_b8, u_b9, u_b10, u_b11, u_b12, u_b13, u_b14, u_b15);\n"
    "  float k[16] = float[16](u_k0.x, u_k0.y, u_k0.z, u_k0.w, u_k1.x, u_k1.y, u_k1.z, u_k1.w, u_k2.x, u_k2.y, u_k2.z, u_k2.w,\n"
    "                          u_k3.x, u_k3.y, u_k3.z, u_k3.w);\n"
    "  float r = -1.0;\n"
    "  for (int i = 0; i < 16; i++) {\n"
    "    if (float(i) >= u_bn.x) break;\n"
    "    if (lo.x < b[i].z && hi.x > b[i].x && lo.y < b[i].w && hi.y > b[i].y) r = max(r, k[i]);\n"
    "  }\n"
    "  return r;\n"
    "}\n"
    "float look_dim(float k) { return (k >= 0.0 && k <= 1.0) ? 1.0 - k : 1.0; }\n"
    /* a display-like brightness from exposed luminance: 0.18 -> about 0.42 */
    "float look_disp(float y) { return pow(max(y, 0.0) / (1.0 + max(y, 0.0)), 1.0 / 2.2); }\n"
    "vec3 look_disp3(vec3 c) { vec3 x = max(c, vec3(0.0)); return pow(x / (vec3(1.0) + x), vec3(1.0 / 2.2)); }\n"
    "vec3 look_undisp3(vec3 d) { vec3 y = min(pow(max(d, vec3(0.0)), vec3(2.2)), vec3(0.999)); return y / (vec3(1.0) - y); }\n"
    "float srgb_lin(float x) { return x <= 0.04045 ? x / 12.92 : pow((x + 0.055) / 1.055, 2.4); }\n"
    /* the 8x8 Bayer matrix's threshold at a cell, (v + 0.5) / 64 */
    "float bayer8(ivec2 p) {\n"
    "  int x = p.x & 7; int y = p.y & 7; int xr = x ^ y;\n"
    "  int v = ((xr & 1) << 5) | ((y & 1) << 4) | ((xr & 2) << 2) | ((y & 2) << 1) | ((xr & 4) >> 1) | ((y & 4) >> 2);\n"
    "  return (float(v) + 0.5) / 64.0;\n"
    "}\n";

#define W_FIELDS "u_rect: vec4f, u_ex: vec4f, u_bn: vec4f, " BOXES_16("u_b", ": vec4f, ") "u_k0: vec4f, u_k1: vec4f, u_k2: vec4f, u_k3: vec4f, "
static const char *W_FUNCS =
    "var<private> FC: vec2f;\n"
    "fn lum(c: vec3f) -> f32 { return dot(c, vec3f(0.2126, 0.7152, 0.0722)); }\n"
    "fn look_E() -> f32 { return u.u_ex.x * exp2(dot(textureLoad(u_auto, vec2i(0), 0).rg, vec2f(1.0, 1.0 / 1024.0))); }\n"
    "fn look_tl() -> vec2f { return vec2f(FC.x - u.u_rect.x, u.u_rect.y + u.u_rect.w - FC.y); }\n"
    "fn look_global(tl: vec2f) -> vec2f { return vec2f(u.u_rect.x + tl.x, u.u_rect.y + u.u_rect.w - tl.y); }\n"
    "fn look_src(tl: vec2f) -> vec3f {\n"
    "  let g = clamp(look_global(tl), u.u_rect.xy + 0.5, u.u_rect.xy + u.u_rect.zw - 0.5);\n"
    "  return look_in(textureSampleLevel(u_src, cw_linear, g / vec2f(textureDimensions(u_src)), 0.0).rgb);\n"
    "}\n"
    "fn look_src_raw(tl: vec2f) -> vec3f {\n"
    "  let q = clamp(vec2i(floor(look_global(tl))), vec2i(u.u_rect.xy), vec2i(u.u_rect.xy + u.u_rect.zw) - 1);\n"
    "  return textureLoad(u_src, q, 0).rgb;\n"
    "}\n"
    "fn look_src_px(tl: vec2f) -> vec3f { return look_in(look_src_raw(tl)); }\n"
    "fn look_box(lo: vec2f, hi: vec2f) -> f32 {\n"
    "  let b = array<vec4f, 16>(" BOXES_16("u.u_b", ",") ");\n"
    "  let k = array<f32, 16>(u.u_k0.x, u.u_k0.y, u.u_k0.z, u.u_k0.w, u.u_k1.x, u.u_k1.y, u.u_k1.z, u.u_k1.w, u.u_k2.x, u.u_k2.y,\n"
    "                         u.u_k2.z, u.u_k2.w, u.u_k3.x, u.u_k3.y, u.u_k3.z, u.u_k3.w);\n"
    "  var r = -1.0;\n"
    "  for (var i = 0; i < 16; i++) {\n"
    "    if (f32(i) >= u.u_bn.x) { break; }\n"
    "    if (lo.x < b[i].z && hi.x > b[i].x && lo.y < b[i].w && hi.y > b[i].y) { r = max(r, k[i]); }\n"
    "  }\n"
    "  return r;\n"
    "}\n"
    "fn look_dim(k: f32) -> f32 { return select(1.0, 1.0 - k, k >= 0.0 && k <= 1.0); }\n"
    "fn look_disp(y0: f32) -> f32 { let y = max(y0, 0.0); return pow(y / (1.0 + y), 1.0 / 2.2); }\n"
    "fn look_disp3(c: vec3f) -> vec3f { let x = max(c, vec3f(0.0)); return pow(x / (vec3f(1.0) + x), vec3f(1.0 / 2.2)); }\n"
    "fn look_undisp3(d: vec3f) -> vec3f { let y = min(pow(max(d, vec3f(0.0)), vec3f(2.2)), vec3f(0.999)); return y / (vec3f(1.0) - y); }\n"
    "fn srgb_lin(x: f32) -> f32 { return select(pow((x + 0.055) / 1.055, 2.4), x / 12.92, x <= 0.04045); }\n"
    "fn bayer8(p: vec2i) -> f32 {\n"
    "  let x = p.x & 7; let y = p.y & 7; let xr = x ^ y;\n"
    "  let v = ((xr & 1) << 5u) | ((y & 1) << 4u) | ((xr & 2) << 2u) | ((y & 2) << 1u) | ((xr & 4) >> 1u) | ((y & 4) >> 2u);\n"
    "  return (f32(v) + 0.5) / 64.0;\n"
    "}\n";

/* The stage's colour spaces (cw_look_stage). look_in: a colour of the image as the look reads it; look_out: the
 * look's colour as the image holds it; look_tsrc / look_tv: a pixel for the dither's brightness and that
 * brightness from its luminance; look_tone4: the tone options' colour (rgb) and brightness (a) from a mean colour.
 * SCENE: the scene-linear image times the exposure, the display-like curve for the dither (as before stages).
 * DISPLAY: the finished picture (sRGB values): read in linear light, written back as display values; the dither
 * takes the displayed colour itself. */
static const char *G_STAGE[2] = {
    "vec3 look_in(vec3 c) { return c; }\n"
    "vec3 look_out(vec3 c) { return c / look_E(); }\n"
    "vec3 look_tsrc(vec2 tl) { return look_src_px(tl); }\n"
    "float look_tv(float y) { return look_disp(y * look_E()); }\n"
    "vec4 look_tone4(vec3 c) { return vec4(look_disp3(c * look_E()), look_disp(lum(c) * look_E())); }\n",
    "vec3 look_in(vec3 c) { vec3 x = clamp(c, 0.0, 1.0); return mix(pow((x + 0.055) / 1.055, vec3(2.4)), x / 12.92, vec3(lessThanEqual(x, vec3(0.04045)))); }\n"
    "vec3 look_out(vec3 c) { vec3 x = clamp(c, 0.0, 1.0); return mix(pow(x, vec3(0.41666)) * 1.055 - vec3(0.055), x * 12.92, vec3(lessThanEqual(x, vec3(0.0031308)))); }\n"
    "vec3 look_tsrc(vec2 tl) { return look_src_raw(tl); }\n"
    "float look_tv(float y) { return clamp(y, 0.0, 1.0); }\n"
    "vec4 look_tone4(vec3 c) { vec3 d = clamp(c, vec3(0.0), vec3(1.0)); return vec4(d, lum(d)); }\n",
};
static const char *W_STAGE[2] = {
    "fn look_in(c: vec3f) -> vec3f { return c; }\n"
    "fn look_out(c: vec3f) -> vec3f { return c / look_E(); }\n"
    "fn look_tsrc(tl: vec2f) -> vec3f { return look_src_px(tl); }\n"
    "fn look_tv(y: f32) -> f32 { return look_disp(y * look_E()); }\n"
    "fn look_tone4(c: vec3f) -> vec4f { return vec4f(look_disp3(c * look_E()), look_disp(lum(c) * look_E())); }\n",
    "fn look_in(c: vec3f) -> vec3f { let x = clamp(c, vec3f(0.0), vec3f(1.0)); return select(pow((x + 0.055) / 1.055, vec3f(2.4)), x / 12.92, x <= vec3f(0.04045)); }\n"
    "fn look_out(c: vec3f) -> vec3f { let x = clamp(c, vec3f(0.0), vec3f(1.0)); return select(pow(x, vec3f(0.41666)) * 1.055 - vec3f(0.055), x * 12.92, x <= vec3f(0.0031308)); }\n"
    "fn look_tsrc(tl: vec2f) -> vec3f { return look_src_raw(tl); }\n"
    "fn look_tv(y: f32) -> f32 { return clamp(y, 0.0, 1.0); }\n"
    "fn look_tone4(c: vec3f) -> vec4f { let d = clamp(c, vec3f(0.0), vec3f(1.0)); return vec4f(d, lum(d)); }\n",
};
static int g_stage; /* the stage being drawn (programs are built for it) */
static const char *const STAGE_SUFFIX[2] = { "", ".display" };

static unsigned build(const char *name0, const char *g_decl, const char *g_body, const char *w_fields, const char *w_decl,
                      const char *w_body) {
  static char buf[65536];
  char name[64];
  snprintf(name, sizeof name, "%s%s", name0, STAGE_SUFFIX[g_stage]);
  int n;
  if (wg())
    n = snprintf(buf, sizeof buf,
                 "struct U { " W_FIELDS "%s };\n"
                 "@group(0) @binding(0) var<uniform> u: U;\n"
                 "@group(0) @binding(1) var u_src: texture_2d<f32>;\n"
                 "@group(0) @binding(2) var u_auto: texture_2d<f32>;\n"
                 "%s%s%s%s",
                 w_fields ? w_fields : "", w_decl ? w_decl : "", W_FUNCS, W_STAGE[g_stage], w_body);
  else
    n = snprintf(buf, sizeof buf, "%s%s%s%s%s", G_COMMON, g_decl ? g_decl : "", G_FUNCS, G_STAGE[g_stage], g_body);
  if (n >= (int)sizeof buf) {
    cw_error("looks: program '%s' is too long", name);
    return 0;
  }
  return wg() ? cw_pass_program_wgsl(name, buf) : cw_pass_program(name, buf);
}

/* ── ASCII ──
 * regions: per character cell, its six regions' brightness (3 rows x 2 columns, sharpened toward the depth edges on
 * the near side), how much of an edge it is, and its mean colour, in three texels a cell side by side (regions 0-3;
 * regions 4-5 and the edge; the colour). With the hold on, a cell whose measures moved only a little since last frame
 * (TAA's jitter, grain, noise: under u_hold.y in brightness and u_hold.z in colour) eases toward the new ones
 * (u_hold.w of the way a frame) instead of taking them: a still picture keeps still measures, a real change passes at
 * once.
 * cells: one texel a character cell (rgb its colour, a the glyph index), its regions matched to the glyph shape
 * vectors (u_gv rows 0-1; row 2: .r the ramp's glyph at each position, .g 1 for a glyph shape matching may pick). A
 * cell keeps the glyph it had last frame (u_prev) unless a new one fits u_par.w times better. */
static const char *ASCII_REG_G_DECL =
    "uniform sampler2D u_depth; uniform sampler2D u_pr; uniform sampler2D u_ps; uniform sampler2D u_pc;\n"
    "uniform vec4 u_cell; uniform vec4 u_lin; uniform vec4 u_par; uniform vec4 u_col; uniform vec4 u_ex2; uniform vec4 u_hold;\n"
    ;
static const char *ASCII_REG_G =
    "float ldepth(vec2 tl) {\n"
    "  if (u_lin.x <= 0.0) return 1.0;\n"
    "  ivec2 q = clamp(ivec2(floor(look_global(tl))), ivec2(u_rect.xy), ivec2(u_rect.xy + u_rect.zw) - 1);\n"
    "  float d = texelFetch(u_depth, q, 0).r;\n"
    "  if (d >= 0.999999) return 1e6;\n"
    "  if (u_lin.z > 0.5) return u_lin.x + d * (u_lin.y - u_lin.x);\n"
    "  return u_lin.x * u_lin.y / (u_lin.y - d * (u_lin.y - u_lin.x));\n"
    "}\n"
    "void main() {\n"
    "  FC = gl_FragCoord.xy;\n"
    "  ivec2 ci = ivec2(FC); int sel = ci.x - (ci.x / 3) * 3; ivec2 cc = ivec2(ci.x / 3, ci.y);\n"
    "  vec2 cs = u_cell.xy; vec2 base = vec2(cc) * cs;\n"
    "  float E = look_E() * u_col.x;\n"
    "  float v[6]; float z[6]; vec3 csum = vec3(0.0); float far = 0.0; float zmax = 0.0;\n"
    "  for (int r = 0; r < 6; r++) {\n"
    "    float col = float(r - (r / 2) * 2), row = float(r / 2);\n"
    "    vec2 c = base + vec2((col + 0.5) * cs.x * 0.5, (row + 0.5) * cs.y / 3.0);\n"
    "    vec2 e = vec2(cs.x * 0.125, cs.y / 12.0);\n"
    "    vec3 t0 = look_src(c + vec2(-e.x, -e.y)), t1 = look_src(c + vec2(e.x, -e.y)), t2 = look_src(c + vec2(-e.x, e.y)), t3 = look_src(c + e);\n"
    "    vec3 mx = t0; if (lum(t1) > lum(mx)) mx = t1; if (lum(t2) > lum(mx)) mx = t2; if (lum(t3) > lum(mx)) mx = t3;\n"
    "    vec3 s = mix((t0 + t1 + t2 + t3) * 0.25, mx, u_ex2.z) * E;\n"
    "    float zz = ldepth(c); z[r] = zz;\n"
    "    if (zz > 1e5) far += 1.0; else zmax = max(zmax, zz);\n"
    "    csum += s;\n"
    "    float l = lum(s); v[r] = l / (1.0 + l);\n"
    "  }\n"
    "  float edged = 0.0;\n"
    "  if (far < 6.0) {\n"
    "    float far_side = (far > 0.0) ? 1e6 : zmax;\n"
    "    for (int r = 0; r < 6; r++) {\n"
    "      if (z[r] > 1e5) continue;\n"
    "      float edge = clamp((log(far_side) - log(z[r])) * u_par.z, 0.0, 1.0);\n"
    "      edged = max(edged, edge);\n"
    "      v[r] = max(v[r], edge * 0.8);\n"
    "    }\n"
    "  }\n"
    "  vec4 a = vec4(v[0], v[1], v[2], v[3]); vec4 b = vec4(v[4], v[5], edged, 1.0); vec4 c = vec4(csum / 6.0, 1.0);\n"
    "  if (u_hold.x > 0.5) {\n"
    "    vec4 pa = texelFetch(u_pr, ivec2(cc.x * 3, cc.y), 0); vec4 pb = texelFetch(u_ps, ivec2(cc.x * 3 + 1, cc.y), 0); vec4 pc = texelFetch(u_pc, ivec2(cc.x * 3 + 2, cc.y), 0);\n"
    "    vec4 da = abs(a - pa); vec2 db = abs(b.xy - pb.xy);\n"
    "    float dv = max(max(max(da.x, da.y), max(da.z, da.w)), max(db.x, db.y));\n"
    "    vec3 dc3 = abs(c.rgb - pc.rgb); float dc = max(dc3.r, max(dc3.g, dc3.b)) / max(max(pc.r, max(pc.g, pc.b)), 0.05);\n"
    "    if (dv < u_hold.y && dc < u_hold.z && abs(b.z - pb.z) < u_hold.y) { a = mix(pa, a, u_hold.w); b = mix(pb, b, u_hold.w); c = mix(pc, c, u_hold.w); }\n"
    "  }\n"
    "  o = sel == 0 ? a : sel == 1 ? b : c;\n"
    "}\n";
static const char *ASCII_REG_W_FIELDS = "u_cell: vec4f, u_lin: vec4f, u_par: vec4f, u_col: vec4f, u_ex2: vec4f, u_hold: vec4f, ";
static const char *ASCII_REG_W_DECL =
    "@group(0) @binding(3) var u_depth: texture_2d<f32>;\n"
    "@group(0) @binding(4) var u_pr: texture_2d<f32>;\n"
    "@group(0) @binding(5) var u_ps: texture_2d<f32>;\n"
    "@group(0) @binding(6) var u_pc: texture_2d<f32>;\n";
static const char *ASCII_REG_W =
    "fn ldepth(tl: vec2f) -> f32 {\n"
    "  if (u.u_lin.x <= 0.0) { return 1.0; }\n"
    "  let q = clamp(vec2i(floor(look_global(tl))), vec2i(u.u_rect.xy), vec2i(u.u_rect.xy + u.u_rect.zw) - 1);\n"
    "  let zl = textureLoad(u_depth, q, 0).r;\n"
    "  if (zl > 1e30) { return 1e6; }\n"
    "  return zl;\n"
    "}\n"
    "@fragment fn fs(@builtin(position) pos: vec4f, @location(0) uv: vec2f) -> @location(0) vec4f {\n"
    "  FC = cw_frag_coord(pos);\n"
    "  let ci = vec2i(FC); let sel = ci.x - (ci.x / 3) * 3; let cc = vec2i(ci.x / 3, ci.y);\n"
    "  let cs = u.u_cell.xy; let base = vec2f(cc) * cs;\n"
    "  let E = look_E() * u.u_col.x;\n"
    "  var v: array<f32, 6>; var z: array<f32, 6>; var csum = vec3f(0.0); var far = 0.0; var zmax = 0.0;\n"
    "  for (var r = 0; r < 6; r++) {\n"
    "    let col = f32(r - (r / 2) * 2); let row = f32(r / 2);\n"
    "    let c = base + vec2f((col + 0.5) * cs.x * 0.5, (row + 0.5) * cs.y / 3.0);\n"
    "    let e = vec2f(cs.x * 0.125, cs.y / 12.0);\n"
    "    let t0 = look_src(c + vec2f(-e.x, -e.y)); let t1 = look_src(c + vec2f(e.x, -e.y)); let t2 = look_src(c + vec2f(-e.x, e.y)); let t3 = look_src(c + e);\n"
    "    var mx = t0; if (lum(t1) > lum(mx)) { mx = t1; } if (lum(t2) > lum(mx)) { mx = t2; } if (lum(t3) > lum(mx)) { mx = t3; }\n"
    "    let s = mix((t0 + t1 + t2 + t3) * 0.25, mx, u.u_ex2.z) * E;\n"
    "    let zz = ldepth(c); z[r] = zz;\n"
    "    if (zz > 1e5) { far += 1.0; } else { zmax = max(zmax, zz); }\n"
    "    csum += s;\n"
    "    let l = lum(s); v[r] = l / (1.0 + l);\n"
    "  }\n"
    "  var edged = 0.0;\n"
    "  if (far < 6.0) {\n"
    "    let far_side = select(zmax, 1e6, far > 0.0);\n"
    "    for (var r = 0; r < 6; r++) {\n"
    "      if (z[r] > 1e5) { continue; }\n"
    "      let edge = clamp((log(far_side) - log(z[r])) * u.u_par.z, 0.0, 1.0);\n"
    "      edged = max(edged, edge);\n"
    "      v[r] = max(v[r], edge * 0.8);\n"
    "    }\n"
    "  }\n"
    "  var a = vec4f(v[0], v[1], v[2], v[3]); var b = vec4f(v[4], v[5], edged, 1.0); var c = vec4f(csum / 6.0, 1.0);\n"
    "  if (u.u_hold.x > 0.5) {\n"
    "    let pa = textureLoad(u_pr, vec2i(cc.x * 3, cc.y), 0); let pb = textureLoad(u_ps, vec2i(cc.x * 3 + 1, cc.y), 0); let pc = textureLoad(u_pc, vec2i(cc.x * 3 + 2, cc.y), 0);\n"
    "    let da = abs(a - pa); let db = abs(b.xy - pb.xy);\n"
    "    let dv = max(max(max(da.x, da.y), max(da.z, da.w)), max(db.x, db.y));\n"
    "    let dc3 = abs(c.rgb - pc.rgb); let dc = max(dc3.r, max(dc3.g, dc3.b)) / max(max(pc.r, max(pc.g, pc.b)), 0.05);\n"
    "    if (dv < u.u_hold.y && dc < u.u_hold.z && abs(b.z - pb.z) < u.u_hold.y) { a = mix(pa, a, u.u_hold.w); b = mix(pb, b, u.u_hold.w); c = mix(pc, c, u.u_hold.w); }\n"
    "  }\n"
    "  if (sel == 0) { return a; }\n"
    "  if (sel == 1) { return b; }\n"
    "  return c;\n"
    "}\n";
static const char *ASCII_CELLS_G_DECL =
    "uniform sampler2D u_r; uniform sampler2D u_s; uniform sampler2D u_c; uniform sampler2D u_gv; uniform sampler2D u_prev;\n"
    "uniform vec4 u_cell; uniform vec4 u_lin; uniform vec4 u_par; uniform vec4 u_col; uniform vec4 u_ext; uniform vec4 u_ex2;\n"
    "uniform vec4 u_tint;\n";
static const char *ASCII_CELLS_G =
    "float gdist(int i, float v[6]) {\n"
    "  vec4 a = texelFetch(u_gv, ivec2(i, 0), 0); vec2 b = texelFetch(u_gv, ivec2(i, 1), 0).rg;\n"
    "  vec4 da = vec4(v[0], v[1], v[2], v[3]) - a; vec2 db = vec2(v[4], v[5]) - b;\n"
    "  return dot(da, da) + dot(db, db);\n"
    "}\n"
    "bool allowed(int i) { return u_ext.x > 0.5 || texelFetch(u_gv, ivec2(i, 2), 0).g > 0.5; }\n"
    "void main() {\n"
    "  FC = gl_FragCoord.xy;\n"
    "  vec2 cell = floor(FC);\n"
    "  vec2 cs = u_cell.xy; vec2 base = cell * cs;\n"
    "  float bk = look_box(base, base + cs);\n"
    "  ivec2 c3 = ivec2(int(cell.x) * 3, int(cell.y));\n"
    "  vec4 ra = texelFetch(u_r, c3, 0); vec4 rb = texelFetch(u_s, c3 + ivec2(1, 0), 0);\n"
    "  float v[6] = float[6](ra.x, ra.y, ra.z, ra.w, rb.x, rb.y); float edged = rb.z;\n"
    "  float m = 0.0; float mn = 1.0; float mean = 0.0;\n"
    "  for (int r = 0; r < 6; r++) { m = max(m, v[r]); mn = min(mn, v[r]); mean += v[r] / 6.0; }\n"
    /* the glyph: a flat cell's from the ramp at its brightness x, any other the shape that fits best. The hold keeps
     * last frame's glyph: a ramp glyph while x stays within its step and (hold - 1) of a step beyond it, a shape while
     * it fits within hold times the best (and the cell is not clearly flat now) */
    "  float g = 0.0; int n = int(u_col.z); int rn = int(u_ext.z);\n"
    "  bool isflat = (m - mn) <= u_ext.y * m && edged < 0.2;\n"
    "  float x = mean * u_ext.w * u_ext.z;\n"
    "  float vs[6]; float best = 1e9; float gs = 0.0;\n"
    "  for (int r = 0; r < 6; r++) vs[r] = m > 0.0 ? pow(v[r] / m, u_par.y) * m : 0.0;\n"
    "  if (m > u_par.x) {\n"
    "    for (int i = 1; i < 95; i++) {\n"
    "      if (i >= n) break;\n"
    "      if (!allowed(i)) continue;\n"
    "      float d = gdist(i, vs);\n"
    "      if (d < best) { best = d; gs = float(i); }\n"
    "    }\n"
    "    g = isflat ? floor(texelFetch(u_gv, ivec2(clamp(int(x), 0, rn - 1), 2), 0).r * 255.0 + 0.5) : gs;\n"
    "  }\n"
    "  if (u_lin.w > 0.5) {\n"
    "    int p = int(texelFetch(u_prev, ivec2(cell), 0).a + 0.5);\n"
    "    if (float(p) != g && p >= 0 && p < n) {\n"
    "      int pr = -1;\n"
    "      for (int i = 0; i < 95; i++) { if (i >= rn) break; if (int(floor(texelFetch(u_gv, ivec2(i, 2), 0).r * 255.0 + 0.5)) == p) { pr = i; break; } }\n"
    "      float h = u_par.w - 1.0;\n"
    "      bool nearflat = isflat || m <= u_par.x || ((m - mn) <= u_ext.y * m * u_par.w && edged < 0.2);\n"
    "      if (pr >= 0 && nearflat && abs(x - float(pr) - 0.5) < 0.5 + h) g = float(p);\n"
    "      else if (p > 0 && allowed(p) && m > u_par.x / u_par.w && (!isflat || (m - mn) > u_ext.y * m / u_par.w) && gdist(p, vs) <= best * u_par.w + 0.004) g = float(p);\n"
    "    }\n"
    "  }\n"
    "  vec3 c = texelFetch(u_c, c3 + ivec2(2, 0), 0).rgb;\n"
    "  float mc = max(max(c.r, c.g), max(c.b, 1e-4));\n"
    "  float mnc = min(min(c.r, c.g), c.b); float key = smoothstep(0.22, 0.5, (mc - mnc) / mc);\n"
    "  vec3 hue = clamp(mix(vec3(lum(c / mc)), c / mc, u_col.y), 0.0, 1.0);\n"
    "  hue /= max(max(hue.r, hue.g), max(hue.b, 1e-4));\n"
    "  hue = mix(u_tint.rgb, hue, key);\n"
    "  float bright = (u_col.w + (1.0 - u_col.w) * clamp(m * 1.4, 0.0, 1.0)) * (isflat ? u_ex2.x : 1.0) * mix(u_ex2.y, 1.0, key) * look_dim(bk);\n"
    "  o = vec4(g > 0.5 ? hue * bright : vec3(0.0), g);\n"
    "}\n";
static const char *ASCII_CELLS_W_FIELDS =
    "u_cell: vec4f, u_lin: vec4f, u_par: vec4f, u_col: vec4f, u_ext: vec4f, u_ex2: vec4f, u_tint: vec4f, ";
static const char *ASCII_CELLS_W_DECL =
    "@group(0) @binding(3) var u_r: texture_2d<f32>;\n"
    "@group(0) @binding(4) var u_s: texture_2d<f32>;\n"
    "@group(0) @binding(5) var u_c: texture_2d<f32>;\n"
    "@group(0) @binding(6) var u_gv: texture_2d<f32>;\n"
    "@group(0) @binding(7) var u_prev: texture_2d<f32>;\n";
static const char *ASCII_CELLS_W =
    "fn gdist(i: i32, v: array<f32, 6>) -> f32 {\n"
    "  let a = textureLoad(u_gv, vec2i(i, 0), 0); let b = textureLoad(u_gv, vec2i(i, 1), 0).rg;\n"
    "  let da = vec4f(v[0], v[1], v[2], v[3]) - a; let db = vec2f(v[4], v[5]) - b;\n"
    "  return dot(da, da) + dot(db, db);\n"
    "}\n"
    "fn allowed(i: i32) -> bool { return u.u_ext.x > 0.5 || textureLoad(u_gv, vec2i(i, 2), 0).g > 0.5; }\n"
    "@fragment fn fs(@builtin(position) pos: vec4f, @location(0) uv: vec2f) -> @location(0) vec4f {\n"
    "  FC = cw_frag_coord(pos);\n"
    "  let cell = floor(FC);\n"
    "  let cs = u.u_cell.xy; let base = cell * cs;\n"
    "  let bk = look_box(base, base + cs);\n"
    "  let c3 = vec2i(i32(cell.x) * 3, i32(cell.y));\n"
    "  let ra = textureLoad(u_r, c3, 0); let rb = textureLoad(u_s, c3 + vec2i(1, 0), 0);\n"
    "  var v = array<f32, 6>(ra.x, ra.y, ra.z, ra.w, rb.x, rb.y); let edged = rb.z;\n"
    "  var m = 0.0; var mn = 1.0; var mean = 0.0;\n"
    "  for (var r = 0; r < 6; r++) { m = max(m, v[r]); mn = min(mn, v[r]); mean += v[r] / 6.0; }\n"
    "  var g = 0; let n = i32(u.u_col.z); let rn = i32(u.u_ext.z);\n"
    "  let isflat = (m - mn) <= u.u_ext.y * m && edged < 0.2;\n"
    "  let x = mean * u.u_ext.w * u.u_ext.z;\n"
    "  var vs: array<f32, 6>; var best = 1e9; var gs = 0;\n"
    "  for (var r = 0; r < 6; r++) { vs[r] = select(0.0, pow(v[r] / m, u.u_par.y) * m, m > 0.0); }\n"
    "  if (m > u.u_par.x) {\n"
    "    for (var i = 1; i < 95; i++) {\n"
    "      if (i >= n) { break; }\n"
    "      if (!allowed(i)) { continue; }\n"
    "      let d = gdist(i, vs);\n"
    "      if (d < best) { best = d; gs = i; }\n"
    "    }\n"
    "    g = select(gs, i32(floor(textureLoad(u_gv, vec2i(clamp(i32(x), 0, rn - 1), 2), 0).r * 255.0 + 0.5)), isflat);\n"
    "  }\n"
    "  if (u.u_lin.w > 0.5) {\n"
    "    let p = i32(textureLoad(u_prev, vec2i(cell), 0).a + 0.5);\n"
    "    if (p != g && p >= 0 && p < n) {\n"
    "      var pr = -1;\n"
    "      for (var i = 0; i < 95; i++) { if (i >= rn) { break; } if (i32(floor(textureLoad(u_gv, vec2i(i, 2), 0).r * 255.0 + 0.5)) == p) { pr = i; break; } }\n"
    "      let h = u.u_par.w - 1.0;\n"
    "      let nearflat = isflat || m <= u.u_par.x || ((m - mn) <= u.u_ext.y * m * u.u_par.w && edged < 0.2);\n"
    "      if (pr >= 0 && nearflat && abs(x - f32(pr) - 0.5) < 0.5 + h) { g = p; }\n"
    "      else if (p > 0 && allowed(p) && m > u.u_par.x / u.u_par.w && (!isflat || (m - mn) > u.u_ext.y * m / u.u_par.w)) {\n"
    "        if (gdist(p, vs) <= best * u.u_par.w + 0.004) { g = p; }\n"
    "      }\n"
    "    }\n"
    "  }\n"
    "  let c = textureLoad(u_c, c3 + vec2i(2, 0), 0).rgb;\n"
    "  let mc = max(max(c.r, c.g), max(c.b, 1e-4));\n"
    "  let mnc = min(min(c.r, c.g), c.b); let key = smoothstep(0.22, 0.5, (mc - mnc) / mc);\n"
    "  var hue = clamp(mix(vec3f(lum(c / mc)), c / mc, u.u_col.y), vec3f(0.0), vec3f(1.0));\n"
    "  hue = hue / max(max(hue.r, hue.g), max(hue.b, 1e-4));\n"
    "  hue = mix(u.u_tint.rgb, hue, key);\n"
    "  let bright = (u.u_col.w + (1.0 - u.u_col.w) * clamp(m * 1.4, 0.0, 1.0)) * select(1.0, u.u_ex2.x, isflat) * mix(u.u_ex2.y, 1.0, key) * look_dim(bk);\n"
    "  return vec4f(select(vec3f(0.0), hue * bright, g > 0), f32(g));\n"
    "}\n";

/* draw: each pixel its cell's glyph from the atlas (the font at the cell's size, glyph i at x = i * cell width, row 0
 * the glyph's top) in the cell's colour times u_glow.z (neon), over a halo of the bright cells round it.
 * u_glow.w = 1 (a gate's probe): the cell's glyph index as the colour, so a tone curve of NONE gives it back as the
 * pixel's 8-bit value */
static const char *ASCII_DRAW_G_DECL = "uniform sampler2D u_cells; uniform sampler2D u_atlas; uniform sampler2D u_gl; uniform vec4 u_cell; uniform vec4 u_glow;\n";
/* glow: at the cells' size, the bright cells' colours blurred over about two cells round them (each pixel then reads
 * it bilinearly: a soft halo of the character's own colour under it) */
static const char *ASCII_GLOW_G_DECL = "uniform sampler2D u_cells; uniform vec4 u_cell; uniform vec4 u_glow;\n";
static const char *ASCII_GLOW_G =
    "void main() {\n"
    "  FC = gl_FragCoord.xy;\n"
    "  vec2 q = floor(FC) + 0.5;\n"
    "  vec3 s = vec3(0.0); float wsum = 0.0;\n"
    "  for (int j = -2; j <= 2; j++) for (int i = -2; i <= 2; i++) {\n"
    "    vec2 d = vec2(float(i), float(j)) * vec2(0.7, 0.45);\n"
    "    float w = exp(-dot(d, d) * 0.9);\n"
    "    vec3 cc = texture(u_cells, (q + d) / u_cell.zw).rgb;\n"
    "    float k = smoothstep(u_glow.y, 1.0, max(cc.r, max(cc.g, cc.b)));\n"
    "    s += cc * k * w; wsum += w;\n"
    "  }\n"
    "  o = vec4(s / wsum * u_glow.x, 1.0);\n"
    "}\n";
static const char *ASCII_GLOW_W_FIELDS = "u_cell: vec4f, u_glow: vec4f, ";
static const char *ASCII_GLOW_W_DECL = "@group(0) @binding(3) var u_cells: texture_2d<f32>;\n";
static const char *ASCII_GLOW_W =
    "@fragment fn fs(@builtin(position) pos: vec4f, @location(0) uv: vec2f) -> @location(0) vec4f {\n"
    "  FC = cw_frag_coord(pos);\n"
    "  let q = floor(FC) + 0.5;\n"
    "  var s = vec3f(0.0); var wsum = 0.0;\n"
    "  for (var j = -2; j <= 2; j++) { for (var i = -2; i <= 2; i++) {\n"
    "    let d = vec2f(f32(i), f32(j)) * vec2f(0.7, 0.45);\n"
    "    let w = exp(-dot(d, d) * 0.9);\n"
    "    let cc = textureSampleLevel(u_cells, cw_linear, (q + d) / u.u_cell.zw, 0.0).rgb;\n"
    "    let k = smoothstep(u.u_glow.y, 1.0, max(cc.r, max(cc.g, cc.b)));\n"
    "    s += cc * k * w; wsum += w;\n"
    "  } }\n"
    "  return vec4f(s / wsum * u.u_glow.x, 1.0);\n"
    "}\n";
static const char *ASCII_DRAW_G =
    "void main() {\n"
    "  FC = gl_FragCoord.xy;\n"
    "  vec2 tl = look_tl();\n"
    "  float bk = look_box(tl, tl);\n"
    "  if (bk > 1.5) { o = vec4(texelFetch(u_src, ivec2(FC), 0).rgb, 1.0); return; }\n"
    "  vec2 c = floor(tl / u_cell.xy);\n"
    "  vec4 cell = texelFetch(u_cells, ivec2(c), 0);\n"
    "  int g = int(cell.a + 0.5);\n"
    "  float E = look_E();\n"
    "  if (u_glow.w > 0.5) { o = vec4(look_out(vec3(srgb_lin(float(g) / 255.0))), 1.0); return; }\n"
    "  ivec2 l = ivec2(tl - c * u_cell.xy);\n"
    "  float cov = texelFetch(u_atlas, ivec2(g * int(u_cell.x) + l.x, l.y), 0).r;\n"
    "  vec3 col = texture(u_gl, tl / u_cell.xy / u_cell.zw).rgb + cell.rgb * cov * u_glow.z;\n"
    "  o = vec4(look_out(col), 1.0);\n"
    "}\n";
static const char *ASCII_DRAW_W_FIELDS = "u_cell: vec4f, u_glow: vec4f, ";
static const char *ASCII_DRAW_W_DECL =
    "@group(0) @binding(3) var u_cells: texture_2d<f32>;\n"
    "@group(0) @binding(4) var u_atlas: texture_2d<f32>;\n"
    "@group(0) @binding(5) var u_gl: texture_2d<f32>;\n";
static const char *ASCII_DRAW_W =
    "@fragment fn fs(@builtin(position) pos: vec4f, @location(0) uv: vec2f) -> @location(0) vec4f {\n"
    "  FC = cw_frag_coord(pos);\n"
    "  let tl = look_tl();\n"
    "  let bk = look_box(tl, tl);\n"
    "  if (bk > 1.5) { return vec4f(textureLoad(u_src, vec2i(FC), 0).rgb, 1.0); }\n"
    "  let c = floor(tl / u.u_cell.xy);\n"
    "  let cell = textureLoad(u_cells, vec2i(c), 0);\n"
    "  let g = i32(cell.a + 0.5);\n"
    "  let E = look_E();\n"
    "  if (u.u_glow.w > 0.5) { return vec4f(look_out(vec3f(srgb_lin(f32(g) / 255.0))), 1.0); }\n"
    "  let l = vec2i(tl - c * u.u_cell.xy);\n"
    "  let cov = textureLoad(u_atlas, vec2i(g * i32(u.u_cell.x) + l.x, l.y), 0).r;\n"
    "  let col = textureSampleLevel(u_gl, cw_linear, tl / u.u_cell.xy / u.u_cell.zw, 0.0).rgb + cell.rgb * cov * u.u_glow.z;\n"
    "  return vec4f(look_out(col), 1.0);\n"
    "}\n";

/* ── HALFBLOCK ──
 * pixels: one texel a pixel of the pair (u_hb.x view pixels square, rows from the top): the exposed scene's mean over
 * the square, quantised to u_hb.y levels per channel in display steps, or to the nearest of u_hb.z palette colours
 * (u_pal: rgb in display steps at x, its linear colour at x on row 1); alpha keeps the palette colour's index.
 * The hold (u_hp.x: last frame's pixels in u_prev): a pixel keeps last frame's palette colour while it is within
 * u_hp.y (display steps) of the nearest, and each channel its level while within u_hp.z of a step past its middle,
 * so a mean hovering on a boundary (TAA's jitter, grain, noise in the finished picture) holds still */
static const char *HB_PX_G_DECL = "uniform sampler2D u_pal; uniform sampler2D u_prev; uniform vec4 u_hb; uniform vec4 u_hp;\n";
static const char *HB_PX_G =
    "void main() {\n"
    "  FC = gl_FragCoord.xy;\n"
    "  vec2 lo = floor(FC) * u_hb.x;\n"
    /* an even square: bilinear reads at the corners of 2 x 2 texel blocks (each the exact mean of four) */
    "  int n = int(u_hb.x); int st = max(1, (n + 15) / 16); vec3 sum = vec3(0.0); float cnt = 0.0;\n"
    "  if (n % 2 == 0 && n <= 32) { for (int j = 0; j < n; j += 2) for (int i = 0; i < n; i += 2) { sum += look_src(lo + vec2(float(i), float(j)) + 1.0); cnt += 1.0; } }\n"
    "  else { for (int j = 0; j < n; j += st) for (int i = 0; i < n; i += st) { sum += look_src_px(lo + vec2(float(i), float(j)) + 0.5); cnt += 1.0; } }\n"
    "  vec3 c = sum / cnt * look_E();\n"
    "  if (u_hb.z > 0.5) {\n"
    "    vec3 d = look_disp3(c); float best = 1e9; int bi = 0;\n"
    "    for (int i = 0; i < 64; i++) {\n"
    "      if (float(i) >= u_hb.z) break;\n"
    "      vec3 e = texelFetch(u_pal, ivec2(i, 0), 0).rgb - d; float dd = dot(e, e);\n"
    "      if (dd < best) { best = dd; bi = i; }\n"
    "    }\n"
    "    if (u_hp.x > 0.5) {\n"
    "      int pi = int(texelFetch(u_prev, ivec2(FC), 0).a * 255.0 + 0.5) - 1;\n"
    "      if (pi >= 0 && float(pi) < u_hb.z) { vec3 e = texelFetch(u_pal, ivec2(pi, 0), 0).rgb - d; if (sqrt(dot(e, e)) <= sqrt(best) + u_hp.y) bi = pi; }\n"
    "    }\n"
    "    o = vec4(texelFetch(u_pal, ivec2(bi, 1), 0).rgb, float(bi + 1) / 255.0);\n"
    "    return;\n"
    "  } else if (u_hb.y > 1.5) {\n"
    "    float L = u_hb.y - 1.0;\n"
    "    vec3 x = look_disp3(c) * L, k = floor(x + 0.5);\n"
    "    if (u_hp.x > 0.5) {\n"
    "      vec3 pk = floor(look_disp3(texelFetch(u_prev, ivec2(FC), 0).rgb) * L + 0.5);\n"
    "      k = mix(k, pk, vec3(lessThanEqual(abs(x - pk), vec3(0.5 + u_hp.z))));\n"
    "    }\n"
    "    c = look_undisp3(k / L);\n"
    "  }\n"
    "  o = vec4(c, 0.0);\n"
    "}\n";
static const char *HB_PX_W_FIELDS = "u_hb: vec4f, u_hp: vec4f, ";
static const char *HB_PX_W_DECL = "@group(0) @binding(3) var u_pal: texture_2d<f32>;\n@group(0) @binding(4) var u_prev: texture_2d<f32>;\n";
static const char *HB_PX_W =
    "@fragment fn fs(@builtin(position) pos: vec4f, @location(0) uv: vec2f) -> @location(0) vec4f {\n"
    "  FC = cw_frag_coord(pos);\n"
    "  let lo = floor(FC) * u.u_hb.x;\n"
    "  let n = i32(u.u_hb.x); let st = max(1, (n + 15) / 16); var sum = vec3f(0.0); var cnt = 0.0;\n"
    "  if (n % 2 == 0 && n <= 32) { for (var j = 0; j < n; j += 2) { for (var i = 0; i < n; i += 2) { sum += look_src(lo + vec2f(f32(i), f32(j)) + 1.0); cnt += 1.0; } } }\n"
    "  else { for (var j = 0; j < n; j += st) { for (var i = 0; i < n; i += st) { sum += look_src_px(lo + vec2f(f32(i), f32(j)) + 0.5); cnt += 1.0; } } }\n"
    "  var c = sum / cnt * look_E();\n"
    "  if (u.u_hb.z > 0.5) {\n"
    "    let d = look_disp3(c); var best = 1e9; var bi = 0;\n"
    "    for (var i = 0; i < 64; i++) {\n"
    "      if (f32(i) >= u.u_hb.z) { break; }\n"
    "      let e = textureLoad(u_pal, vec2i(i, 0), 0).rgb - d; let dd = dot(e, e);\n"
    "      if (dd < best) { best = dd; bi = i; }\n"
    "    }\n"
    "    if (u.u_hp.x > 0.5) {\n"
    "      let pi = i32(textureLoad(u_prev, vec2i(FC), 0).a * 255.0 + 0.5) - 1;\n"
    "      if (pi >= 0 && f32(pi) < u.u_hb.z) { let e = textureLoad(u_pal, vec2i(pi, 0), 0).rgb - d; if (sqrt(dot(e, e)) <= sqrt(best) + u.u_hp.y) { bi = pi; } }\n"
    "    }\n"
    "    return vec4f(textureLoad(u_pal, vec2i(bi, 1), 0).rgb, f32(bi + 1) / 255.0);\n"
    "  } else if (u.u_hb.y > 1.5) {\n"
    "    let L = u.u_hb.y - 1.0;\n"
    "    let x = look_disp3(c) * L; var k = floor(x + 0.5);\n"
    "    if (u.u_hp.x > 0.5) {\n"
    "      let pk = floor(look_disp3(textureLoad(u_prev, vec2i(FC), 0).rgb) * L + 0.5);\n"
    "      k = select(k, pk, abs(x - pk) <= vec3f(0.5 + u.u_hp.z));\n"
    "    }\n"
    "    c = look_undisp3(k / L);\n"
    "  }\n"
    "  return vec4f(c, 0.0);\n"
    "}\n";
/* draw: each view pixel takes its pixel pair texel (top half of a cell the upper one) */
static const char *HB_DRAW_G_DECL = "uniform sampler2D u_cells; uniform vec4 u_hb;\n";
static const char *HB_DRAW_G =
    "void main() {\n"
    "  FC = gl_FragCoord.xy;\n"
    "  vec2 tl = look_tl();\n"
    "  float bk = look_box(tl, tl);\n"
    "  if (bk > 1.5) { o = vec4(texelFetch(u_src, ivec2(FC), 0).rgb, 1.0); return; }\n"
    "  vec3 c = texelFetch(u_cells, ivec2(floor(tl / u_hb.x)), 0).rgb;\n"
    "  o = vec4(look_out(c * look_dim(bk)), 1.0);\n"
    "}\n";
static const char *HB_DRAW_W_FIELDS = "u_hb: vec4f, ";
static const char *HB_DRAW_W_DECL = "@group(0) @binding(3) var u_cells: texture_2d<f32>;\n";
static const char *HB_DRAW_W =
    "@fragment fn fs(@builtin(position) pos: vec4f, @location(0) uv: vec2f) -> @location(0) vec4f {\n"
    "  FC = cw_frag_coord(pos);\n"
    "  let tl = look_tl();\n"
    "  let bk = look_box(tl, tl);\n"
    "  if (bk > 1.5) { return vec4f(textureLoad(u_src, vec2i(FC), 0).rgb, 1.0); }\n"
    "  let c = textureLoad(u_cells, vec2i(floor(tl / u.u_hb.x)), 0).rgb;\n"
    "  return vec4f(look_out(c * look_dim(bk)), 1.0);\n"
    "}\n";

/* ── DITHER ──
 * the brightness of a dither pixel (u_d.x view pixels square): the exposed luminance's mean over it, through
 * look_disp, then contrast (u_d.y) and bias (u_d.z). u_ex.y offsets the pattern (0; the crawl control moves it). */
#define DITHER_V_G                                                                                                            \
  "float dither_v(vec2 b) {\n"                                                                                                \
  "  vec2 lo = b * u_d.x; int n = int(u_d.x); int st = max(1, (n + 7) / 8);\n"                                                 \
  "  float sum = 0.0; float cnt = 0.0;\n"                                                                                     \
  "  for (int j = 0; j < n; j += st) for (int i = 0; i < n; i += st) { sum += lum(look_tsrc(lo + vec2(float(i), float(j)) + 0.5)); cnt += 1.0; }\n" \
  "  float v = look_tv(sum / cnt);\n"                                                                            \
  "  return clamp((v - 0.5) * u_d.y + 0.5 + u_d.z, 0.0, 1.0);\n"                                                              \
  "}\n"                                                                                                                       \
  "float dither_t(vec2 b) {\n"                                                                                                \
  "  ivec2 p = ivec2(b) + ivec2(int(u_ex.y), int(u_ex.y) * 3);\n"                                                             \
  "  if (u_d.w > 0.5) return (texelFetch(u_noise, p & 63, 0).r * 255.0 + 0.5) / 256.0;\n"                                     \
  "  return bayer8(p);\n"                                                                                                     \
  "}\n"
#define DITHER_V_W                                                                                                            \
  "fn dither_v(b: vec2f) -> f32 {\n"                                                                                          \
  "  let lo = b * u.u_d.x; let n = i32(u.u_d.x); let st = max(1, (n + 7) / 8);\n"                                              \
  "  var sum = 0.0; var cnt = 0.0;\n"                                                                                         \
  "  for (var j = 0; j < n; j += st) { for (var i = 0; i < n; i += st) { sum += lum(look_tsrc(lo + vec2f(f32(i), f32(j)) + 0.5)); cnt += 1.0; } }\n" \
  "  let v = look_tv(sum / cnt);\n"                                                                              \
  "  return clamp((v - 0.5) * u.u_d.y + 0.5 + u.u_d.z, 0.0, 1.0);\n"                                                          \
  "}\n"                                                                                                                       \
  "fn dither_t(b: vec2f) -> f32 {\n"                                                                                          \
  "  let p = vec2i(b) + vec2i(i32(u.u_ex.y), i32(u.u_ex.y) * 3);\n"                                                           \
  "  if (u.u_d.w > 0.5) { return (textureLoad(u_noise, p & vec2i(63), 0).r * 255.0 + 0.5) / 256.0; }\n"                       \
  "  return bayer8(p);\n"                                                                                                     \
  "}\n"
/* ordered: in one draw at the view's size */
static const char *DITH_ORD_G_DECL = "uniform sampler2D u_noise; uniform vec4 u_d; uniform vec4 u_dark; uniform vec4 u_light;\n";
static const char *DITH_ORD_G =
    DITHER_V_G
    "void main() {\n"
    "  FC = gl_FragCoord.xy;\n"
    "  vec2 tl = look_tl();\n"
    "  float bk = look_box(tl, tl);\n"
    "  if (bk > 1.5) { o = vec4(texelFetch(u_src, ivec2(FC), 0).rgb, 1.0); return; }\n"
    "  vec2 b = floor(tl / u_d.x);\n"
    "  vec3 c = dither_v(b) > dither_t(b) ? u_light.rgb : u_dark.rgb;\n"
    "  o = vec4(look_out(c * look_dim(bk)), 1.0);\n"
    "}\n";
static const char *DITH_W_FIELDS = "u_d: vec4f, u_dark: vec4f, u_light: vec4f, ";
static const char *DITH_ORD_W_DECL = "@group(0) @binding(3) var u_noise: texture_2d<f32>;\n";
static const char *DITH_ORD_W =
    DITHER_V_W
    "@fragment fn fs(@builtin(position) pos: vec4f, @location(0) uv: vec2f) -> @location(0) vec4f {\n"
    "  FC = cw_frag_coord(pos);\n"
    "  let tl = look_tl();\n"
    "  let bk = look_box(tl, tl);\n"
    "  if (bk > 1.5) { return vec4f(textureLoad(u_src, vec2i(FC), 0).rgb, 1.0); }\n"
    "  let b = floor(tl / u.u_d.x);\n"
    "  let c = select(u.u_dark.rgb, u.u_light.rgb, dither_v(b) > dither_t(b));\n"
    "  return vec4f(look_out(c * look_dim(bk)), 1.0);\n"
    "}\n";
/* diffuse, 1: the state, one texel a dither pixel: r its brightness, g its bit from the blue noise threshold */
static const char *DITH_INIT_G =
    DITHER_V_G
    "void main() {\n"
    "  FC = gl_FragCoord.xy;\n"
    "  vec2 b = floor(FC);\n"
    "  float v = dither_v(b);\n"
    "  o = vec4(v, v > dither_t(b) ? 1.0 : 0.0, 0.0, 1.0);\n"
    "}\n";
static const char *DITH_INIT_W =
    DITHER_V_W
    "@fragment fn fs(@builtin(position) pos: vec4f, @location(0) uv: vec2f) -> @location(0) vec4f {\n"
    "  FC = cw_frag_coord(pos);\n"
    "  let b = floor(FC);\n"
    "  let v = dither_v(b);\n"
    "  return vec4f(v, select(0.0, 1.0, v > dither_t(b)), 0.0, 1.0);\n"
    "}\n";
/* diffuse, 2: one phase of a direct binary search sweep. A pixel of the phase (its x and y parity u_s.xy) turns on or
 * off when that lowers the error between the picture and the bits, both seen through a Gaussian (sigma 0.92 pixels);
 * the flip's change of that error is d^2 c(0) - 2 d (c * e)(p), c the filter's autocorrelation (sigma 1.3, 5x5, c(0) = 1)
 * and e = brightness - bit: it flips on when (c * e) > 1/2 and off when it is under -1/2. */
static const char *DITH_DBS_G_DECL = "uniform sampler2D u_state; uniform vec4 u_s;\n";
static const char *DITH_DBS_G =
    "void main() {\n"
    "  FC = gl_FragCoord.xy;\n"
    "  ivec2 p = ivec2(FC);\n"
    "  vec2 st = texelFetch(u_state, p, 0).rg;\n"
    "  if ((p.x & 1) != int(u_s.x) || (p.y & 1) != int(u_s.y)) { o = vec4(st, 0.0, 1.0); return; }\n"
    "  ivec2 sz = textureSize(u_state, 0);\n"
    /* the 5x5 kernel: exp(-(i^2 + j^2) / (2 1.3^2)), by squared distance */
    "  float ce = 0.0;\n"
    "  for (int j = -2; j <= 2; j++) for (int i = -2; i <= 2; i++) {\n"
    "    int r2 = i * i + j * j; float w = r2 == 0 ? 1.0 : r2 == 1 ? 0.7438 : r2 == 2 ? 0.5533 : r2 == 4 ? 0.3061 : r2 == 5 ? 0.2277 : 0.0937;\n"
    "    vec2 t = texelFetch(u_state, clamp(p + ivec2(i, j), ivec2(0), sz - 1), 0).rg;\n"
    "    ce += w * (t.r - t.g);\n"
    "  }\n"
    "  float b = st.g;\n"
    "  if (b < 0.5 && ce > 0.5) b = 1.0;\n"
    "  else if (b > 0.5 && ce < -0.5) b = 0.0;\n"
    "  o = vec4(st.r, b, 0.0, 1.0);\n"
    "}\n";
static const char *DITH_DBS_W_FIELDS = "u_s: vec4f, ";
static const char *DITH_DBS_W_DECL = "@group(0) @binding(3) var u_state: texture_2d<f32>;\n";
static const char *DITH_DBS_W =
    "@fragment fn fs(@builtin(position) pos: vec4f, @location(0) uv: vec2f) -> @location(0) vec4f {\n"
    "  FC = cw_frag_coord(pos);\n"
    "  let p = vec2i(FC);\n"
    "  let st = textureLoad(u_state, p, 0).rg;\n"
    "  if ((p.x & 1) != i32(u.u_s.x) || (p.y & 1) != i32(u.u_s.y)) { return vec4f(st, 0.0, 1.0); }\n"
    "  let sz = vec2i(textureDimensions(u_state));\n"
    "  var ce = 0.0;\n"
    "  for (var j = -2; j <= 2; j++) { for (var i = -2; i <= 2; i++) {\n"
    "    let r2 = i * i + j * j; let w = select(select(select(select(select(0.0937, 0.2277, r2 == 5), 0.3061, r2 == 4), 0.5533, r2 == 2), 0.7438, r2 == 1), 1.0, r2 == 0);\n"
    "    let t = textureLoad(u_state, clamp(p + vec2i(i, j), vec2i(0), sz - 1), 0).rg;\n"
    "    ce += w * (t.r - t.g);\n"
    "  } }\n"
    "  var b = st.g;\n"
    "  if (b < 0.5 && ce > 0.5) { b = 1.0; } else if (b > 0.5 && ce < -0.5) { b = 0.0; }\n"
    "  return vec4f(st.r, b, 0.0, 1.0);\n"
    "}\n";
/* diffuse, 3: the bits drawn in the two colours */
static const char *DITH_DRAW_G_DECL = "uniform sampler2D u_state; uniform vec4 u_d; uniform vec4 u_dark; uniform vec4 u_light;\n";
static const char *DITH_DRAW_G =
    "void main() {\n"
    "  FC = gl_FragCoord.xy;\n"
    "  vec2 tl = look_tl();\n"
    "  float bk = look_box(tl, tl);\n"
    "  if (bk > 1.5) { o = vec4(texelFetch(u_src, ivec2(FC), 0).rgb, 1.0); return; }\n"
    "  float bit = texelFetch(u_state, ivec2(floor(tl / u_d.x)), 0).g;\n"
    "  vec3 c = bit > 0.5 ? u_light.rgb : u_dark.rgb;\n"
    "  o = vec4(look_out(c * look_dim(bk)), 1.0);\n"
    "}\n";
static const char *DITH_DRAW_W_DECL = "@group(0) @binding(3) var u_state: texture_2d<f32>;\n";
static const char *DITH_DRAW_W =
    "@fragment fn fs(@builtin(position) pos: vec4f, @location(0) uv: vec2f) -> @location(0) vec4f {\n"
    "  FC = cw_frag_coord(pos);\n"
    "  let tl = look_tl();\n"
    "  let bk = look_box(tl, tl);\n"
    "  if (bk > 1.5) { return vec4f(textureLoad(u_src, vec2i(FC), 0).rgb, 1.0); }\n"
    "  let bit = textureLoad(u_state, vec2i(floor(tl / u.u_d.x)), 0).g;\n"
    "  let c = select(u.u_dark.rgb, u.u_light.rgb, bit > 0.5);\n"
    "  return vec4f(look_out(c * look_dim(bk)), 1.0);\n"
    "}\n";


/* the tone options (cw_look_dither: hue_value onward): dither_L is a dither pixel's brightness from its mean colour
 * (look_tone4: rgb and the plain brightness at this stage), saturated colours by their value (u_t0.x gain; u_t0.yz
 * the saturation's smoothstep), grey by its brightness times u_t0.w with the highlights (u_t1.xy) kept; dither_tone
 * adds the unsharp mask (u_t1.z gain, u_t1.w radius in dither pixels), gamma (u_t2.x), contrast and bias (u_d.yz) and
 * the depth outlines (u_t2.y brightness, u_t2.zw the relative Laplacian of 1 / depth's smoothstep), from the depth
 * at each dither pixel's middle (u_lin: near, far, orthographic, depth bound). Neighbours are taken in the order
 * a bottom-left anchored pass takes them (+x, -x, up, down), so a pixel's sums come out the same. */
#define DITHER_L_G                                                                                                         \
  "float dither_L(vec2 b) {\n"                                                                                               \
  "  vec2 lo = b * u_d.x; int n = int(u_d.x); int st = max(1, (n + 7) / 8);\n"                                               \
  "  vec3 sum = vec3(0.0); float cnt = 0.0;\n"                                                                              \
  "  for (int j = 0; j < n; j += st) for (int i = 0; i < n; i += st) { sum += look_tsrc(lo + vec2(float(i), float(j)) + 0.5); cnt += 1.0; }\n" \
  "  vec4 t = look_tone4(sum / cnt);\n"                                                                                     \
  "  float mx = max(max(t.r, t.g), t.b), mn = min(min(t.r, t.g), t.b);\n"                                                  \
  "  float key = u_t0.x > 0.0 ? smoothstep(u_t0.y, u_t0.z, (mx - mn) / max(mx, 1e-4)) : 0.0;\n"                            \
  "  float l = t.a;\n"                                                                                                       \
  "  float g = u_t0.w != 1.0 ? mix(l * u_t0.w, l, smoothstep(u_t1.x, u_t1.y, l)) : l;\n"                                    \
  "  return clamp(mix(g, mx * u_t0.x, key), 0.0, 1.0);\n"                                                                  \
  "}\n"
#define DITHER_SHAPE_G(L)                                                                                                    \
  "float dither_W(vec2 b) {\n"                                                                                               \
  "  if (u_lin.w < 0.5) return 0.0;\n"                                                                                      \
  "  vec2 tl = b * u_d.x + floor(u_d.x * 0.5) + 0.5;\n"                                                                     \
  "  ivec2 q = clamp(ivec2(floor(look_global(tl))), ivec2(u_rect.xy), ivec2(u_rect.xy + u_rect.zw) - 1);\n"                 \
  "  float d = texelFetch(u_depth, q, 0).r;\n"                                                                              \
  "  if (d >= 0.999999) return 0.0;\n"                                                                                      \
  "  float z = u_lin.z > 0.5 ? u_lin.x + d * (u_lin.y - u_lin.x) : u_lin.x * u_lin.y / (u_lin.y - d * (u_lin.y - u_lin.x));\n" \
  "  return 1.0 / max(z, 1e-4);\n"                                                                                          \
  "}\n"                                                                                                                      \
  "float dither_tone(vec2 b) {\n"                                                                                            \
  "  float c = " L "(b);\n"                                                                                              \
  "  if (u_t1.z > 0.0) {\n"                                                                                                 \
  "    float r = u_t1.w; float h = max(1.0, floor(r * 0.5 + 0.5));\n"                                                      \
  "    float bl = (" L "(b + vec2(r, 0.0)) + " L "(b - vec2(r, 0.0)) + " L "(b - vec2(0.0, r)) + " L "(b + vec2(0.0, r))) * 0.15\n" \
  "             + (" L "(b + vec2(h, -h)) + " L "(b - vec2(h, -h)) + " L "(b + vec2(h, h)) + " L "(b - vec2(h, h))) * 0.1;\n" \
  "    c = clamp(c + u_t1.z * (c - bl), 0.0, 1.0);\n"                                                                      \
  "  }\n"                                                                                                                    \
  "  float v = u_t2.x != 1.0 ? pow(c, u_t2.x) : c;\n"                                                                      \
  "  v = clamp((v - 0.5) * u_d.y + 0.5 + u_d.z, 0.0, 1.0);\n"                                                               \
  "  if (u_t2.y > 0.0) {\n"                                                                                                 \
  "    float w = dither_W(b);\n"                                                                                            \
  "    if (w > 0.0) {\n"                                                                                                    \
  "      float lap = 4.0 * w - dither_W(b + vec2(1.0, 0.0)) - dither_W(b - vec2(1.0, 0.0)) - dither_W(b - vec2(0.0, 1.0)) - dither_W(b + vec2(0.0, 1.0));\n" \
  "      v = max(v, smoothstep(u_t2.z, u_t2.w, lap / w) * u_t2.y);\n"                                                      \
  "    }\n"                                                                                                                  \
  "  }\n"                                                                                                                    \
  "  return v;\n"                                                                                                            \
  "}\n"
#define DITHER_TONE_G DITHER_L_G DITHER_SHAPE_G("dither_L")
#define DITHER_L_W                                                                                                         \
  "fn dither_L(b: vec2f) -> f32 {\n"                                                                                         \
  "  let lo = b * u.u_d.x; let n = i32(u.u_d.x); let st = max(1, (n + 7) / 8);\n"                                            \
  "  var sum = vec3f(0.0); var cnt = 0.0;\n"                                                                                \
  "  for (var j = 0; j < n; j += st) { for (var i = 0; i < n; i += st) { sum += look_tsrc(lo + vec2f(f32(i), f32(j)) + 0.5); cnt += 1.0; } }\n" \
  "  let t = look_tone4(sum / cnt);\n"                                                                                      \
  "  let mx = max(max(t.r, t.g), t.b); let mn = min(min(t.r, t.g), t.b);\n"                                                \
  "  var key = 0.0;\n"                                                                                                       \
  "  if (u.u_t0.x > 0.0) { key = smoothstep(u.u_t0.y, u.u_t0.z, (mx - mn) / max(mx, 1e-4)); }\n"                          \
  "  let l = t.a;\n"                                                                                                         \
  "  var g = l;\n"                                                                                                           \
  "  if (u.u_t0.w != 1.0) { g = mix(l * u.u_t0.w, l, smoothstep(u.u_t1.x, u.u_t1.y, l)); }\n"                              \
  "  return clamp(mix(g, mx * u.u_t0.x, key), 0.0, 1.0);\n"                                                                \
  "}\n"
#define DITHER_SHAPE_W(L)                                                                                                    \
  "fn dither_W(b: vec2f) -> f32 {\n"                                                                                         \
  "  if (u.u_lin.w < 0.5) { return 0.0; }\n"                                                                                \
  "  let tl = b * u.u_d.x + floor(u.u_d.x * 0.5) + 0.5;\n"                                                                  \
  "  let q = clamp(vec2i(floor(look_global(tl))), vec2i(u.u_rect.xy), vec2i(u.u_rect.xy + u.u_rect.zw) - 1);\n"             \
  "  let z = textureLoad(u_depth, q, 0).r;\n"                                                                               \
  "  if (z > 1e30) { return 0.0; }\n"                                                                                       \
  "  return 1.0 / max(z, 1e-4);\n"                                                                                          \
  "}\n"                                                                                                                      \
  "fn dither_tone(b: vec2f) -> f32 {\n"                                                                                      \
  "  var c = " L "(b);\n"                                                                                                \
  "  if (u.u_t1.z > 0.0) {\n"                                                                                               \
  "    let r = u.u_t1.w; let h = max(1.0, floor(r * 0.5 + 0.5));\n"                                                        \
  "    let bl = (" L "(b + vec2f(r, 0.0)) + " L "(b - vec2f(r, 0.0)) + " L "(b - vec2f(0.0, r)) + " L "(b + vec2f(0.0, r))) * 0.15\n" \
  "           + (" L "(b + vec2f(h, -h)) + " L "(b - vec2f(h, -h)) + " L "(b + vec2f(h, h)) + " L "(b - vec2f(h, h))) * 0.1;\n" \
  "    c = clamp(c + u.u_t1.z * (c - bl), 0.0, 1.0);\n"                                                                    \
  "  }\n"                                                                                                                    \
  "  var v = c;\n"                                                                                                           \
  "  if (u.u_t2.x != 1.0) { v = pow(c, u.u_t2.x); }\n"                                                                     \
  "  v = clamp((v - 0.5) * u.u_d.y + 0.5 + u.u_d.z, 0.0, 1.0);\n"                                                           \
  "  if (u.u_t2.y > 0.0) {\n"                                                                                               \
  "    let w = dither_W(b);\n"                                                                                              \
  "    if (w > 0.0) {\n"                                                                                                    \
  "      let lap = 4.0 * w - dither_W(b + vec2f(1.0, 0.0)) - dither_W(b - vec2f(1.0, 0.0)) - dither_W(b - vec2f(0.0, 1.0)) - dither_W(b + vec2f(0.0, 1.0));\n" \
  "      v = max(v, smoothstep(u.u_t2.z, u.u_t2.w, lap / w) * u.u_t2.y);\n"                                              \
  "    }\n"                                                                                                                  \
  "  }\n"                                                                                                                    \
  "  return v;\n"                                                                                                            \
  "}\n"
#define DITHER_TONE_W DITHER_L_W DITHER_SHAPE_W("dither_L")
/* the ordered and the diffused start with the tone options (their own programs: without them the ones above run) */
static const char *DITH_T_G_DECL =
    "uniform sampler2D u_noise; uniform sampler2D u_depth; uniform vec4 u_d; uniform vec4 u_dark; uniform vec4 u_light;\n"
    "uniform vec4 u_t0; uniform vec4 u_t1; uniform vec4 u_t2; uniform vec4 u_lin;\n";
static const char *DITH_ORD_T_G =
    DITHER_V_G DITHER_TONE_G
    "void main() {\n"
    "  FC = gl_FragCoord.xy;\n"
    "  vec2 tl = look_tl();\n"
    "  float bk = look_box(tl, tl);\n"
    "  if (bk > 1.5) { o = vec4(texelFetch(u_src, ivec2(FC), 0).rgb, 1.0); return; }\n"
    "  vec2 b = floor(tl / u_d.x);\n"
    "  vec3 c = dither_tone(b) > dither_t(b) ? u_light.rgb : u_dark.rgb;\n"
    "  o = vec4(look_out(c * look_dim(bk)), 1.0);\n"
    "}\n";
static const char *DITH_INIT_T_G =
    DITHER_V_G DITHER_TONE_G
    "void main() {\n"
    "  FC = gl_FragCoord.xy;\n"
    "  vec2 b = floor(FC);\n"
    "  float v = dither_tone(b);\n"
    "  o = vec4(v, v > dither_t(b) ? 1.0 : 0.0, 0.0, 1.0);\n"
    "}\n";
static const char *DITH_T_W_FIELDS = "u_d: vec4f, u_dark: vec4f, u_light: vec4f, u_t0: vec4f, u_t1: vec4f, u_t2: vec4f, u_lin: vec4f, ";
static const char *DITH_T_W_DECL =
    "@group(0) @binding(3) var u_noise: texture_2d<f32>;\n"
    "@group(0) @binding(4) var u_depth: texture_2d<f32>;\n";
static const char *DITH_ORD_T_W =
    DITHER_V_W DITHER_TONE_W
    "@fragment fn fs(@builtin(position) pos: vec4f, @location(0) uv: vec2f) -> @location(0) vec4f {\n"
    "  FC = cw_frag_coord(pos);\n"
    "  let tl = look_tl();\n"
    "  let bk = look_box(tl, tl);\n"
    "  if (bk > 1.5) { return vec4f(textureLoad(u_src, vec2i(FC), 0).rgb, 1.0); }\n"
    "  let b = floor(tl / u.u_d.x);\n"
    "  let c = select(u.u_dark.rgb, u.u_light.rgb, dither_tone(b) > dither_t(b));\n"
    "  return vec4f(look_out(c * look_dim(bk)), 1.0);\n"
    "}\n";
static const char *DITH_INIT_T_W =
    DITHER_V_W DITHER_TONE_W
    "@fragment fn fs(@builtin(position) pos: vec4f, @location(0) uv: vec2f) -> @location(0) vec4f {\n"
    "  FC = cw_frag_coord(pos);\n"
    "  let b = floor(FC);\n"
    "  let v = dither_tone(b);\n"
    "  return vec4f(v, select(0.0, 1.0, v > dither_t(b)), 0.0, 1.0);\n"
    "}\n";

/* The tone options once a dither pixel, in small targets (the programs above evaluate dither_tone in every screen
 * pixel, so a dither pixel of n x n screen pixels took its mean colour n^2 times and the unsharp mask nine times
 * that). 1: dither_L of every dither pixel the mask reaches, into a target u_lp.x dither pixels larger on each side
 * (texel = dither pixel + u_lp.x); 2: dither_tone from those (the ordered: into a target of a texel a dither pixel;
 * the diffused: its state directly); 3 (ordered): the threshold in each screen pixel. Values travel as 24 bit fixed
 * point in RGBA8 (2^-24: below any threshold step), so the picture is the one the per-pixel programs draw. */
#define DITHER_PACK_G                                                                                                       \
  "vec4 pack24(float v) {\n"                                                                                                 \
  "  float x = floor(clamp(v, 0.0, 1.0) * 16777215.0 + 0.5); float r = floor(x / 65536.0); x -= r * 65536.0;\n"              \
  "  float g = floor(x / 256.0); return vec4(r, g, x - g * 256.0, 255.0) / 255.0;\n"                                         \
  "}\n"                                                                                                                      \
  "float unpack24(vec4 c) { vec3 b = floor(c.rgb * 255.0 + 0.5); return (b.r * 65536.0 + b.g * 256.0 + b.b) / 16777215.0; }\n" \
  "float dither_Lt(vec2 b) { return unpack24(texelFetch(u_ltex, ivec2(b + u_lp.x), 0)); }\n"
#define DITHER_PACK_W                                                                                                       \
  "fn pack24(v: f32) -> vec4f {\n"                                                                                           \
  "  var x = floor(clamp(v, 0.0, 1.0) * 16777215.0 + 0.5); let r = floor(x / 65536.0); x -= r * 65536.0;\n"                  \
  "  let g = floor(x / 256.0); return vec4f(r, g, x - g * 256.0, 255.0) / 255.0;\n"                                          \
  "}\n"                                                                                                                      \
  "fn unpack24(c: vec4f) -> f32 { let b = floor(c.rgb * 255.0 + 0.5); return (b.r * 65536.0 + b.g * 256.0 + b.b) / 16777215.0; }\n" \
  "fn dither_Lt(b: vec2f) -> f32 { return unpack24(textureLoad(u_ltex, vec2i(b + u.u_lp.x), 0)); }\n"
static const char *DITH_S_G_DECL =
    "uniform sampler2D u_noise; uniform sampler2D u_depth; uniform sampler2D u_ltex; uniform vec4 u_d; uniform vec4 u_dark;\n"
    "uniform vec4 u_light; uniform vec4 u_t0; uniform vec4 u_t1; uniform vec4 u_t2; uniform vec4 u_lin; uniform vec4 u_lp;\n";
static const char *DITH_S_W_FIELDS = "u_d: vec4f, u_dark: vec4f, u_light: vec4f, u_t0: vec4f, u_t1: vec4f, u_t2: vec4f, u_lin: vec4f, u_lp: vec4f, ";
static const char *DITH_S_W_DECL =
    "@group(0) @binding(3) var u_noise: texture_2d<f32>;\n"
    "@group(0) @binding(4) var u_depth: texture_2d<f32>;\n"
    "@group(0) @binding(5) var u_ltex: texture_2d<f32>;\n";
static const char *DITH_SL_G =
    DITHER_PACK_G DITHER_L_G
    "void main() {\n"
    "  FC = gl_FragCoord.xy;\n"
    "  o = pack24(dither_L(floor(FC) - u_lp.x));\n"
    "}\n";
static const char *DITH_SL_W =
    DITHER_PACK_W DITHER_L_W
    "@fragment fn fs(@builtin(position) pos: vec4f, @location(0) uv: vec2f) -> @location(0) vec4f {\n"
    "  FC = cw_frag_coord(pos);\n"
    "  return pack24(dither_L(floor(FC) - u.u_lp.x));\n"
    "}\n";
static const char *DITH_ST_G =
    DITHER_PACK_G DITHER_SHAPE_G("dither_Lt")
    "void main() {\n"
    "  FC = gl_FragCoord.xy;\n"
    "  o = pack24(dither_tone(floor(FC)));\n"
    "}\n";
static const char *DITH_ST_W =
    DITHER_PACK_W DITHER_SHAPE_W("dither_Lt")
    "@fragment fn fs(@builtin(position) pos: vec4f, @location(0) uv: vec2f) -> @location(0) vec4f {\n"
    "  FC = cw_frag_coord(pos);\n"
    "  return pack24(dither_tone(floor(FC)));\n"
    "}\n";
/* the ordered's screen pixels: u_ltex the tone a dither pixel */
static const char *DITH_SO_G =
    DITHER_V_G DITHER_PACK_G
    "void main() {\n"
    "  FC = gl_FragCoord.xy;\n"
    "  vec2 tl = look_tl();\n"
    "  float bk = look_box(tl, tl);\n"
    "  if (bk > 1.5) { o = vec4(texelFetch(u_src, ivec2(FC), 0).rgb, 1.0); return; }\n"
    "  vec2 b = floor(tl / u_d.x);\n"
    "  vec3 c = unpack24(texelFetch(u_ltex, ivec2(b), 0)) > dither_t(b) ? u_light.rgb : u_dark.rgb;\n"
    "  o = vec4(look_out(c * look_dim(bk)), 1.0);\n"
    "}\n";
static const char *DITH_SO_W =
    DITHER_V_W DITHER_PACK_W
    "@fragment fn fs(@builtin(position) pos: vec4f, @location(0) uv: vec2f) -> @location(0) vec4f {\n"
    "  FC = cw_frag_coord(pos);\n"
    "  let tl = look_tl();\n"
    "  let bk = look_box(tl, tl);\n"
    "  if (bk > 1.5) { return vec4f(textureLoad(u_src, vec2i(FC), 0).rgb, 1.0); }\n"
    "  let b = floor(tl / u.u_d.x);\n"
    "  let c = select(u.u_dark.rgb, u.u_light.rgb, unpack24(textureLoad(u_ltex, vec2i(b), 0)) > dither_t(b));\n"
    "  return vec4f(look_out(c * look_dim(bk)), 1.0);\n"
    "}\n";
/* the diffused's state from the dither pixels' brightness (u_ltex as in 2) */
static const char *DITH_SI_G =
    DITHER_V_G DITHER_PACK_G DITHER_SHAPE_G("dither_Lt")
    "void main() {\n"
    "  FC = gl_FragCoord.xy;\n"
    "  vec2 b = floor(FC);\n"
    "  float v = dither_tone(b);\n"
    "  o = vec4(v, v > dither_t(b) ? 1.0 : 0.0, 0.0, 1.0);\n"
    "}\n";
static const char *DITH_SI_W =
    DITHER_V_W DITHER_PACK_W DITHER_SHAPE_W("dither_Lt")
    "@fragment fn fs(@builtin(position) pos: vec4f, @location(0) uv: vec2f) -> @location(0) vec4f {\n"
    "  FC = cw_frag_coord(pos);\n"
    "  let b = floor(FC);\n"
    "  let v = dither_tone(b);\n"
    "  return vec4f(v, select(0.0, 1.0, v > dither_t(b)), 0.0, 1.0);\n"
    "}\n";

/* ── CUSTOM ── the game's look() wrapped: its parameters (u_p0..3), the boxes, the exposure; the depth (unit 2, u_lin:
 * near, far, orthographic, depth bound) and the blue noise (unit 3) for the accessors */
static const char *CUSTOM_G_DECL = "uniform vec4 u_p0; uniform vec4 u_p1; uniform vec4 u_p2; uniform vec4 u_p3; uniform vec4 u_lin;\n"
                                   "uniform sampler2D u_depth; uniform sampler2D u_noise;\n";
static const char *CUSTOM_G_HELP =
    "vec2 cw_look_size() { return u_rect.zw; }\n"
    "vec3 cw_look_scene(vec2 px) { return look_src(px) * look_E(); }\n"
    "float cw_look_scale() { return u_ex.z; }\n"
    "float cw_look_time() { return u_ex.w; }\n"
    "float cw_look_frame() { return u_bn.y; }\n"
    "vec4 cw_look_param(int i) { return i == 0 ? u_p0 : i == 1 ? u_p1 : i == 2 ? u_p2 : u_p3; }\n"
    "float cw_look_stage() { return u_bn.z; }\n"
    "float cw_look_depth(vec2 px) {\n"
    "  if (u_lin.w < 0.5) return 1.0;\n"
    "  ivec2 q = clamp(ivec2(floor(look_global(px))), ivec2(u_rect.xy), ivec2(u_rect.xy + u_rect.zw) - 1);\n"
    "  return texelFetch(u_depth, q, 0).r;\n"
    "}\n"
    "float cw_look_linear_depth(vec2 px) {\n"
    "  float d = cw_look_depth(px);\n"
    "  if (d >= 0.999999) return 3.0e38;\n"
    "  return u_lin.z > 0.5 ? u_lin.x + d * (u_lin.y - u_lin.x) : u_lin.x * u_lin.y / (u_lin.y - d * (u_lin.y - u_lin.x));\n"
    "}\n"
    "float cw_look_bayer(vec2 px) { return bayer8(ivec2(floor(px))); }\n"
    "float cw_look_blue_noise(vec2 px) { return (texelFetch(u_noise, ivec2(floor(px)) & 63, 0).r * 255.0 + 0.5) / 256.0; }\n"
    "vec3 cw_look_from_display(vec3 d) { vec3 x = clamp(d, 0.0, 1.0); return mix(pow((x + 0.055) / 1.055, vec3(2.4)), x / 12.92, vec3(lessThanEqual(x, vec3(0.04045)))); }\n"
    "vec3 cw_look_to_display(vec3 c) { vec3 x = clamp(c, 0.0, 1.0); return mix(pow(x, vec3(0.41666)) * 1.055 - vec3(0.055), x * 12.92, vec3(lessThanEqual(x, vec3(0.0031308)))); }\n";
static const char *CUSTOM_G_MAIN =
    "void main() {\n"
    "  FC = gl_FragCoord.xy;\n"
    "  vec2 tl = look_tl();\n"
    "  float bk = look_box(tl, tl);\n"
    "  if (bk > 1.5) { o = vec4(texelFetch(u_src, ivec2(FC), 0).rgb, 1.0); return; }\n"
    "  o = vec4(look_out(max(look(tl), vec3(0.0)) * look_dim(bk)), 1.0);\n"
    "}\n";
static const char *CUSTOM_W_FIELDS = "u_p0: vec4f, u_p1: vec4f, u_p2: vec4f, u_p3: vec4f, u_lin: vec4f, ";
static const char *CUSTOM_W_DECL =
    "@group(0) @binding(3) var u_depth: texture_2d<f32>;\n"
    "@group(0) @binding(4) var u_noise: texture_2d<f32>;\n";
static const char *CUSTOM_W_HELP =
    "fn cw_look_size() -> vec2f { return u.u_rect.zw; }\n"
    "fn cw_look_scene(px: vec2f) -> vec3f { return look_src(px) * look_E(); }\n"
    "fn cw_look_scale() -> f32 { return u.u_ex.z; }\n"
    "fn cw_look_time() -> f32 { return u.u_ex.w; }\n"
    "fn cw_look_frame() -> f32 { return u.u_bn.y; }\n"
    "fn cw_look_param(i: i32) -> vec4f { if (i == 0) { return u.u_p0; } if (i == 1) { return u.u_p1; } if (i == 2) { return u.u_p2; } return u.u_p3; }\n"
    "fn cw_look_stage() -> f32 { return u.u_bn.z; }\n"
    "fn cw_look_linear_depth(px: vec2f) -> f32 {\n"
    "  if (u.u_lin.w < 0.5) { return 3.0e38; }\n"
    "  let q = clamp(vec2i(floor(look_global(px))), vec2i(u.u_rect.xy), vec2i(u.u_rect.xy + u.u_rect.zw) - 1);\n"
    "  let z = textureLoad(u_depth, q, 0).r;\n"
    "  if (z > 1e30) { return 3.0e38; }\n"
    "  return z;\n"
    "}\n"
    "fn cw_look_depth(px: vec2f) -> f32 {\n"
    "  let z = cw_look_linear_depth(px);\n"
    "  if (z > 1e30) { return 1.0; }\n"
    "  if (u.u_lin.z > 0.5) { return (z - u.u_lin.x) / (u.u_lin.y - u.u_lin.x); }\n"
    "  return u.u_lin.y * (z - u.u_lin.x) / (z * (u.u_lin.y - u.u_lin.x));\n"
    "}\n"
    "fn cw_look_bayer(px: vec2f) -> f32 { return bayer8(vec2i(floor(px))); }\n"
    "fn cw_look_blue_noise(px: vec2f) -> f32 { return (textureLoad(u_noise, vec2i(floor(px)) & vec2i(63), 0).r * 255.0 + 0.5) / 256.0; }\n"
    "fn cw_look_from_display(d: vec3f) -> vec3f { let x = clamp(d, vec3f(0.0), vec3f(1.0)); return select(pow((x + 0.055) / 1.055, vec3f(2.4)), x / 12.92, x <= vec3f(0.04045)); }\n"
    "fn cw_look_to_display(c: vec3f) -> vec3f { let x = clamp(c, vec3f(0.0), vec3f(1.0)); return select(pow(x, vec3f(0.41666)) * 1.055 - vec3f(0.055), x * 12.92, x <= vec3f(0.0031308)); }\n";
static const char *CUSTOM_W_MAIN =
    "@fragment fn fs(@builtin(position) pos: vec4f, @location(0) uv: vec2f) -> @location(0) vec4f {\n"
    "  FC = cw_frag_coord(pos);\n"
    "  let tl = look_tl();\n"
    "  let bk = look_box(tl, tl);\n"
    "  if (bk > 1.5) { return vec4f(textureLoad(u_src, vec2i(FC), 0).rgb, 1.0); }\n"
    "  return vec4f(look_out(max(look(tl), vec3f(0.0)) * look_dim(bk)), 1.0);\n"
    "}\n";

/* ── the blend of two looks' pictures (units 2 and 3) ──
 * u_mix: t, blend (0 fade, 1 dissolve, 2 wipe), softness, noise grain (view pixels); u_dir: the wipe's direction, the
 * dissolve's seed. The edge passes each place at f (0..1): w = 1 - smoothstep(e - s, e + s, f), e = t (1 + 2 s) - s, so
 * t = 0 is all a and t = 1 all b at any softness. */
static const char *MIX_G_DECL = "uniform sampler2D u_a; uniform sampler2D u_b; uniform vec4 u_mix; uniform vec4 u_dir;\n";
static const char *MIX_G =
    "float h01(ivec2 p, int s) {\n"
    "  uvec3 v = uvec3(uvec2(p + 4096), uint(s)) * 1664525u + 1013904223u;\n"
    "  v.x += v.y * v.z; v.y += v.z * v.x; v.z += v.x * v.y; v ^= v >> 16u;\n"
    "  v.x += v.y * v.z; v.y += v.z * v.x; v.z += v.x * v.y;\n"
    "  return float(v.x >> 8u) / 16777216.0;\n"
    "}\n"
    "float vnoise(vec2 q, int s) {\n"
    "  vec2 i = floor(q); vec2 f = q - i; f = f * f * (3.0 - 2.0 * f); ivec2 k = ivec2(i);\n"
    "  return mix(mix(h01(k, s), h01(k + ivec2(1, 0), s), f.x), mix(h01(k + ivec2(0, 1), s), h01(k + ivec2(1, 1), s), f.x), f.y);\n"
    "}\n"
    "void main() {\n"
    "  FC = gl_FragCoord.xy;\n"
    "  vec2 tl = look_tl();\n"
    "  vec3 a = texelFetch(u_a, ivec2(FC), 0).rgb; vec3 b = texelFetch(u_b, ivec2(FC), 0).rgb;\n"
    "  float t = u_mix.x; float s = u_mix.z; float w = t;\n"
    "  if (u_mix.y > 0.5) {\n"
    "    float f;\n"
    "    if (u_mix.y < 1.5) {\n"
    "      vec2 q = tl / max(u_mix.w, 1.0); int sd = int(u_dir.z);\n"
    "      f = clamp((vnoise(q, sd) * 0.65 + vnoise(q * 2.03 + 17.0, sd + 1) * 0.35 - 0.5) * 1.6 + 0.5, 0.0, 1.0);\n"
    "    } else {\n"
    "      vec2 pb = vec2(tl.x, u_rect.w - tl.y); vec2 d = u_dir.xy;\n"
    "      float lo = min(0.0, d.x * u_rect.z) + min(0.0, d.y * u_rect.w), hi = max(0.0, d.x * u_rect.z) + max(0.0, d.y * u_rect.w);\n"
    "      f = (dot(pb, d) - lo) / max(hi - lo, 1e-4);\n"
    "    }\n"
    "    float e = t * (1.0 + 2.0 * s) - s;\n"
    "    w = s < 1e-4 ? (f < t ? 1.0 : 0.0) : 1.0 - smoothstep(e - s, e + s, f);\n"
    "  }\n"
    "  o = vec4(mix(a, b, w), 1.0);\n"
    "}\n";
static const char *MIX_W_FIELDS = "u_mix: vec4f, u_dir: vec4f, ";
static const char *MIX_W_DECL =
    "@group(0) @binding(3) var u_a: texture_2d<f32>;\n"
    "@group(0) @binding(4) var u_b: texture_2d<f32>;\n";
static const char *MIX_W =
    "fn h01(p: vec2i, s: i32) -> f32 {\n"
    "  var v = vec3u(vec2u(p + vec2i(4096)), u32(s)) * 1664525u + vec3u(1013904223u);\n"
    "  v.x += v.y * v.z; v.y += v.z * v.x; v.z += v.x * v.y; v ^= v >> vec3u(16u);\n"
    "  v.x += v.y * v.z; v.y += v.z * v.x; v.z += v.x * v.y;\n"
    "  return f32(v.x >> 8u) / 16777216.0;\n"
    "}\n"
    "fn vnoise(q: vec2f, s: i32) -> f32 {\n"
    "  let i = floor(q); var f = q - i; f = f * f * (3.0 - 2.0 * f); let k = vec2i(i);\n"
    "  return mix(mix(h01(k, s), h01(k + vec2i(1, 0), s), f.x), mix(h01(k + vec2i(0, 1), s), h01(k + vec2i(1, 1), s), f.x), f.y);\n"
    "}\n"
    "@fragment fn fs(@builtin(position) pos: vec4f, @location(0) uv: vec2f) -> @location(0) vec4f {\n"
    "  FC = cw_frag_coord(pos);\n"
    "  let tl = look_tl();\n"
    "  let a = textureLoad(u_a, vec2i(FC), 0).rgb; let b = textureLoad(u_b, vec2i(FC), 0).rgb;\n"
    "  let t = u.u_mix.x; let s = u.u_mix.z; var w = t;\n"
    "  if (u.u_mix.y > 0.5) {\n"
    "    var f: f32;\n"
    "    if (u.u_mix.y < 1.5) {\n"
    "      let q = tl / max(u.u_mix.w, 1.0); let sd = i32(u.u_dir.z);\n"
    "      f = clamp((vnoise(q, sd) * 0.65 + vnoise(q * 2.03 + 17.0, sd + 1) * 0.35 - 0.5) * 1.6 + 0.5, 0.0, 1.0);\n"
    "    } else {\n"
    "      let pb = vec2f(tl.x, u.u_rect.w - tl.y); let d = u.u_dir.xy;\n"
    "      let lo = min(0.0, d.x * u.u_rect.z) + min(0.0, d.y * u.u_rect.w); let hi = max(0.0, d.x * u.u_rect.z) + max(0.0, d.y * u.u_rect.w);\n"
    "      f = (dot(pb, d) - lo) / max(hi - lo, 1e-4);\n"
    "    }\n"
    "    let e = t * (1.0 + 2.0 * s) - s;\n"
    "    if (s < 1e-4) { w = select(0.0, 1.0, f < t); } else { w = 1.0 - smoothstep(e - s, e + s, f); }\n"
    "  }\n"
    "  return vec4f(mix(a, b, w), 1.0);\n"
    "}\n";

/* ── brightness: the mean exposed luminance of a look's picture in the view, reduced 8 x 8 at a time; the last step
 * packs it into an RGBA8 texel (value / 64 as 16 bits in r and g) for the read back ── */
static const char *LUM_G_DECL = "uniform sampler2D u_img; uniform vec4 u_lm; uniform vec4 u_lf;\n";
static const char *LUM_G =
    "void main() {\n"
    "  FC = gl_FragCoord.xy;\n"
    "  ivec2 p = ivec2(FC) * 8; float s = 0.0; float n = 0.0;\n"
    "  ivec2 lo = ivec2(u_lm.zw); ivec2 hi = ivec2(u_lm.xy);\n"
    "  for (int j = 0; j < 8; j++) for (int i = 0; i < 8; i++) {\n"
    "    ivec2 q = p + ivec2(i, j);\n"
    "    if (q.x >= hi.x - lo.x || q.y >= hi.y - lo.y) continue;\n"
    "    vec4 c = texelFetch(u_img, q + lo, 0);\n"
    "    s += u_lf.x > 0.5 ? lum(look_in(c.rgb)) * look_E() * c.a : c.r * c.a; n += c.a;\n"
    "  }\n"
    "  float m = n > 0.0 ? s / n : 0.0;\n"
    "  if (u_lf.y > 0.5) { float v = clamp(m / 64.0, 0.0, 1.0) * 65535.0; float hi8 = floor(v / 256.0); o = vec4(hi8 / 255.0, (v - hi8 * 256.0) / 255.0, 0.0, 1.0); }\n"
    "  else o = vec4(m, 0.0, 0.0, n > 0.0 ? 1.0 : 0.0);\n"
    "}\n";
static const char *LUM_W_FIELDS = "u_lm: vec4f, u_lf: vec4f, ";
static const char *LUM_W_DECL = "@group(0) @binding(3) var u_img: texture_2d<f32>;\n";
static const char *LUM_W =
    "@fragment fn fs(@builtin(position) pos: vec4f, @location(0) uv: vec2f) -> @location(0) vec4f {\n"
    "  FC = cw_frag_coord(pos);\n"
    "  let p = vec2i(FC) * 8; var s = 0.0; var n = 0.0;\n"
    "  let lo = vec2i(u.u_lm.zw); let hi = vec2i(u.u_lm.xy);\n"
    "  for (var j = 0; j < 8; j++) { for (var i = 0; i < 8; i++) {\n"
    "    let q = p + vec2i(i, j);\n"
    "    if (q.x >= hi.x - lo.x || q.y >= hi.y - lo.y) { continue; }\n"
    "    let c = textureLoad(u_img, q + lo, 0);\n"
    "    s += select(c.r * c.a, lum(look_in(c.rgb)) * look_E() * c.a, u.u_lf.x > 0.5); n += c.a;\n"
    "  } }\n"
    "  let m = select(0.0, s / n, n > 0.0);\n"
    "  if (u.u_lf.y > 0.5) { let v = clamp(m / 64.0, 0.0, 1.0) * 65535.0; let hi8 = floor(v / 256.0); return vec4f(hi8 / 255.0, (v - hi8 * 256.0) / 255.0, 0.0, 1.0); }\n"
    "  return vec4f(m, 0.0, 0.0, select(0.0, 1.0, n > 0.0));\n"
    "}\n";

enum { P_ASCII_REG, P_ASCII_CELLS, P_ASCII_GLOW, P_ASCII_DRAW, P_HB_PX, P_HB_DRAW, P_DITH_ORD, P_DITH_INIT, P_DITH_DBS, P_DITH_DRAW, P_MIX, P_LUM,
       P_DITH_ORD_T, P_DITH_INIT_T, P_DITH_SL, P_DITH_ST, P_DITH_SO, P_DITH_SI, P_COUNT };
static unsigned PROG[2][2][P_COUNT]; /* [backend][stage][program] */
static bool PROG_TRIED[2][2][P_COUNT]; /* (a program that failed to build is not tried again every frame) */

static unsigned prog(int k) {
  int b = wg() ? 1 : 0;
  if (PROG[b][g_stage][k] || PROG_TRIED[b][g_stage][k]) return PROG[b][g_stage][k];
  PROG_TRIED[b][g_stage][k] = true;
  CW_PROFILE_SCOPE("look.program");
  unsigned p = 0;
  switch (k) {
  case P_ASCII_REG: p = build("look.ascii_regions", ASCII_REG_G_DECL, ASCII_REG_G, ASCII_REG_W_FIELDS, ASCII_REG_W_DECL, ASCII_REG_W); break;
  case P_ASCII_CELLS: p = build("look.ascii_cells", ASCII_CELLS_G_DECL, ASCII_CELLS_G, ASCII_CELLS_W_FIELDS, ASCII_CELLS_W_DECL, ASCII_CELLS_W); break;
  case P_ASCII_GLOW: p = build("look.ascii_glow", ASCII_GLOW_G_DECL, ASCII_GLOW_G, ASCII_GLOW_W_FIELDS, ASCII_GLOW_W_DECL, ASCII_GLOW_W); break;
  case P_ASCII_DRAW: p = build("look.ascii_draw", ASCII_DRAW_G_DECL, ASCII_DRAW_G, ASCII_DRAW_W_FIELDS, ASCII_DRAW_W_DECL, ASCII_DRAW_W); break;
  case P_HB_PX: p = build("look.halfblock_px", HB_PX_G_DECL, HB_PX_G, HB_PX_W_FIELDS, HB_PX_W_DECL, HB_PX_W); break;
  case P_HB_DRAW: p = build("look.halfblock_draw", HB_DRAW_G_DECL, HB_DRAW_G, HB_DRAW_W_FIELDS, HB_DRAW_W_DECL, HB_DRAW_W); break;
  case P_DITH_ORD: p = build("look.dither_ordered", DITH_ORD_G_DECL, DITH_ORD_G, DITH_W_FIELDS, DITH_ORD_W_DECL, DITH_ORD_W); break;
  case P_DITH_INIT: p = build("look.dither_init", DITH_ORD_G_DECL, DITH_INIT_G, DITH_W_FIELDS, DITH_ORD_W_DECL, DITH_INIT_W); break;
  case P_DITH_DBS: p = build("look.dither_dbs", DITH_DBS_G_DECL, DITH_DBS_G, DITH_DBS_W_FIELDS, DITH_DBS_W_DECL, DITH_DBS_W); break;
  case P_DITH_DRAW: p = build("look.dither_draw", DITH_DRAW_G_DECL, DITH_DRAW_G, DITH_W_FIELDS, DITH_DRAW_W_DECL, DITH_DRAW_W); break;
  case P_MIX: p = build("look.mix", MIX_G_DECL, MIX_G, MIX_W_FIELDS, MIX_W_DECL, MIX_W); break;
  case P_LUM: p = build("look.luminance", LUM_G_DECL, LUM_G, LUM_W_FIELDS, LUM_W_DECL, LUM_W); break;
  case P_DITH_ORD_T: p = build("look.dither_ordered_tone", DITH_T_G_DECL, DITH_ORD_T_G, DITH_T_W_FIELDS, DITH_T_W_DECL, DITH_ORD_T_W); break;
  case P_DITH_INIT_T: p = build("look.dither_init_tone", DITH_T_G_DECL, DITH_INIT_T_G, DITH_T_W_FIELDS, DITH_T_W_DECL, DITH_INIT_T_W); break;
  case P_DITH_SL: p = build("look.dither_tone_l", DITH_S_G_DECL, DITH_SL_G, DITH_S_W_FIELDS, DITH_S_W_DECL, DITH_SL_W); break;
  case P_DITH_ST: p = build("look.dither_tone", DITH_S_G_DECL, DITH_ST_G, DITH_S_W_FIELDS, DITH_S_W_DECL, DITH_ST_W); break;
  case P_DITH_SO: p = build("look.dither_ordered_toned", DITH_S_G_DECL, DITH_SO_G, DITH_S_W_FIELDS, DITH_S_W_DECL, DITH_SO_W); break;
  case P_DITH_SI: p = build("look.dither_init_toned", DITH_S_G_DECL, DITH_SI_G, DITH_S_W_FIELDS, DITH_S_W_DECL, DITH_SI_W); break;
  default: break;
  }
  PROG[b][g_stage][k] = p;
  return p;
}

/* ── shared textures ─────────────────────────────────────────────────── */

/* a 1x1 automatic exposure state of 0 stops (the setting off) */
static unsigned neutral_tex(void) {
  static unsigned tex;
  if (!tex) {
    static const uint16_t zero[2] = { 0, 0 };
    tex = cw_pass_texture_new(1, 1, CW_RT_RG16F, zero);
  }
  return tex;
}

/* 64 x 64 blue noise by void and cluster (Ulichney 1993; a Gaussian of sigma 1.5 on the torus), as ranks scaled to
 * 0..255: a table (look_blue_noise.h, made by tools/gen-look-blue-noise.c and held to the void and cluster on every
 * host by unit_looks), uploaded the first time a look needs it. Up to v0.66.0 it was computed here, which cost 46 ms of
 * CPU in a cart on the first frame a dither look drew. */
#define BN 64
static unsigned blue_noise_tex(void) {
  static unsigned tex;
  if (!tex) tex = cw_pass_texture_new(BN, BN, CW_RT_R8, cw__look_blue_noise);
  return tex;
}

/* ── ASCII resources ─────────────────────────────────────────────────── */

static bool ascii_prepare(look *l) {
  if (l->ascii_ver == l->ver && l->gv_tex) return l->fnt != NULL;
  CW_PROFILE_SCOPE("look.ascii_prepare");
  if (!l->fnt) {
    if (l->font) {
      size_t len = 0;
      void *ttf = cw_vfs_read(l->font, &len);
      l->fnt = ttf ? cw__look_font_new(ttf, len, ttf) : NULL;
      if (!l->fnt) {
        free(ttf);
        cw_error("cw_look: no font at %s (an ASCII look); the built-in Fira Mono draws it", l->font);
      }
    }
    if (!l->fnt) {
      size_t len = 0;
      const unsigned char *ttf = cw__ui_mono_ttf(&len);
      l->fnt = cw__look_font_new(ttf, len, NULL);
    }
    if (!l->fnt) return false;
    l->aspect = cw__look_font_aspect(l->fnt);
    for (int i = 0; i < 4; i++) cw_pass_texture_free(l->atlas[i].tex), l->atlas[i] = (look_atlas){ 0 };
  }
  /* the glyph shape vectors: six regions (3 rows x 2 columns, top row first, left column first) measured on a large
   * raster, normalized so the inkiest region of any glyph is 1 */
  const int ch = 96, cw = (int)lrintf(96 * l->aspect);
  unsigned char *b = cw_alloc((size_t)cw * ch, CW_MEM_RENDER);
  float v[NGLYPH][6], mx = 0;
  for (int i = 0; i < NGLYPH; i++) {
    cw__look_font_raster(l->fnt, FIRST_CP + i, cw, ch, b);
    for (int r = 0; r < 6; r++) {
      int col = r & 1, row = r >> 1;
      int xa = col * cw / 2, xb = (col + 1) * cw / 2, ya = row * ch / 3, yb = (row + 1) * ch / 3;
      double s = 0;
      for (int y = ya; y < yb; y++)
        for (int x = xa; x < xb; x++) s += b[y * cw + x];
      v[i][r] = (float)(s / (255.0 * (xb - xa) * (yb - ya)));
      if (v[i][r] > mx) mx = v[i][r];
    }
  }
  cw_free(b);
  unsigned char tex[3][NGLYPH][4];
  memset(tex, 0, sizeof tex);
  for (int i = 0; i < NGLYPH; i++)
    for (int r = 0; r < 6; r++) tex[r / 4][i][r % 4] = (unsigned char)lrintf((mx > 0 ? v[i][r] / mx : 0) * 255.0f);
  /* row 2: .r the ramp (glyph indices, darkest first), .g the glyphs shape matching may pick */
  const char *ramp = l->ramp && *l->ramp ? l->ramp : DEF_RAMP, *gl = l->glyphs ? l->glyphs : DEF_GLYPHS;
  int rn = 0;
  for (const char *c = ramp; *c && rn < NGLYPH; c++)
    if ((unsigned char)*c >= FIRST_CP && (unsigned char)*c < FIRST_CP + NGLYPH) tex[2][rn++][0] = (unsigned char)(*c - FIRST_CP);
  if (!rn) tex[2][rn++][0] = 0;
  for (const char *c = gl; *c; c++)
    if ((unsigned char)*c > FIRST_CP && (unsigned char)*c < FIRST_CP + NGLYPH) tex[2][*c - FIRST_CP][1] = 255;
  l->ramp_n = rn;
  cw_pass_texture_free(l->gv_tex);
  l->gv_tex = cw_pass_texture_new(NGLYPH, 3, CW_RT_RGBA8, tex);
  l->ascii_ver = l->ver;
  return l->gv_tex != 0;
}

/* the atlas at a cell size: NGLYPH cells across, row 0 the glyphs' top row, strokes made a little bolder */
static unsigned ascii_atlas(look *l, int cw, int ch) {
  l->atlas_clock++;
  int lru = 0;
  for (int i = 0; i < 4; i++) {
    if (l->atlas[i].tex && l->atlas[i].cw == cw && l->atlas[i].ch == ch) {
      l->atlas[i].used = l->atlas_clock;
      return l->atlas[i].tex;
    }
    if (l->atlas[i].used < l->atlas[lru].used) lru = i;
  }
  CW_PROFILE_SCOPE("look.ascii_atlas");
  unsigned char *cell = cw_alloc((size_t)cw * ch, CW_MEM_RENDER), *tex = cw_calloc((size_t)cw * NGLYPH * ch, 1, CW_MEM_RENDER);
  for (int i = 0; i < NGLYPH; i++) {
    cw__look_font_raster(l->fnt, FIRST_CP + i, cw, ch, cell);
    for (int y = 0; y < ch; y++) {
      unsigned char *row = tex + (size_t)y * cw * NGLYPH + (size_t)i * cw;
      for (int x = 0; x < cw; x++) {
        int v = cell[y * cw + x], k = x > 0 ? cell[y * cw + x - 1] * 4 / 5 : 0;
        row[x] = (unsigned char)(v > k ? v : k);
      }
    }
  }
  cw_pass_texture_free(l->atlas[lru].tex);
  l->atlas[lru] = (look_atlas){ cw, ch, cw_pass_texture_new(cw * NGLYPH, ch, CW_RT_R8, tex), l->atlas_clock };
  cw_free(cell), cw_free(tex);
  return l->atlas[lru].tex;
}

/* the halfblock palette: row 0 each colour in display steps, row 1 its linear value (RGBA16F halves) */
static unsigned palette_tex(look *l) {
  int n = l->d.halfblock.palette_count;
  if (!n) return neutral_tex();
  if (l->pal_tex && l->pal_ver == l->ver) return l->pal_tex;
  uint16_t px[2][CW_LOOK_MAX_PALETTE][4];
  memset(px, 0, sizeof px);
  for (int i = 0; i < n; i++)
    for (int c = 0; c < 3; c++) {
      float y = CW_MAX(l->pal[i * 3 + c], 0.0f);
      px[0][i][c] = lm_float_to_half(powf(y / (1.0f + y), 1.0f / 2.2f));
      px[1][i][c] = lm_float_to_half(y);
    }
  for (int i = 0; i < n; i++) px[0][i][3] = px[1][i][3] = lm_float_to_half(1.0f);
  /* (the texture is CW_LOOK_MAX_PALETTE wide: rows packed) */
  cw_pass_texture_free(l->pal_tex);
  l->pal_tex = cw_pass_texture_new(CW_LOOK_MAX_PALETTE, 2, CW_RT_RGBA16F, px);
  l->pal_ver = l->ver;
  return l->pal_tex;
}

/* ── the pass ────────────────────────────────────────────────────────── */

typedef struct vinfo {
  int rect[4];        /* the view in the target: x, y from the bottom left, w, h */
  float out_h;        /* the output's height in pixels (sizes are shares of it) */
  float scale;        /* out_h / 1080 */
  int slot;           /* the view's history slot */
  int nbox;
  float box[CW_LOOK_MAX_BOXES][4]; /* view pixels from the top left: x0, y0, x1, y1 */
  float boxk[CW_LOOK_MAX_BOXES];
  bool first;         /* the view the brightness is measured in */
} vinfo;

static void view_info(const cw_pass_ctx *ctx, const looks_state *s, vinfo *v) {
  const cw_pipeline *p = ctx->pipeline;
  float vf[4] = { 0, 0, 1, 1 };
  memset(v, 0, sizeof *v);
  bool boxes = true;
  if (ctx->view_count > 0) {
    memcpy(v->rect, ctx->view_rect, sizeof v->rect);
    v->out_h = (float)ctx->height;
    v->slot = ctx->view_index;
    if (ctx->view_index >= 0 && ctx->view_index < p->nviews)
      vf[0] = p->views[ctx->view_index].x, vf[1] = p->views[ctx->view_index].y, vf[2] = p->views[ctx->view_index].w,
      vf[3] = p->views[ctx->view_index].h;
  } else {
    v->rect[0] = v->rect[1] = 0, v->rect[2] = ctx->width, v->rect[3] = ctx->height;
    v->out_h = (float)ctx->height;
    if (p->rt_scope) { /* a SubViewport's or another extra view's run: a picture of its own */
      v->slot = 8 + (p->view_i & 7);
      boxes = false;
    } else if (p->nviews >= 2 && p->view_i >= 0 && p->view_i < p->nviews) {
      const cw_view *w = &p->views[p->view_i];
      vf[0] = w->x, vf[1] = w->y, vf[2] = w->w, vf[3] = w->h;
      v->slot = p->view_i;
      v->out_h = w->h > 0 ? (float)ctx->height / w->h : (float)ctx->height;
    }
  }
  v->first = v->slot == 0;
  v->scale = v->out_h / 1080.0f;
  if (!boxes || vf[2] <= 0 || vf[3] <= 0) return;
  for (int i = 0; i < s->nbox; i++) {
    const cw_look_box *b = &s->box[i];
    float x0 = (b->x - vf[0]) / vf[2] * (float)v->rect[2], x1 = (b->x + b->w - vf[0]) / vf[2] * (float)v->rect[2];
    float y0 = (b->y - vf[1]) / vf[3] * (float)v->rect[3], y1 = (b->y + b->h - vf[1]) / vf[3] * (float)v->rect[3];
    if (x1 <= 0 || y1 <= 0 || x0 >= (float)v->rect[2] || y0 >= (float)v->rect[3] || x1 <= x0 || y1 <= y0) continue;
    float *o = v->box[v->nbox];
    o[0] = x0, o[1] = y0, o[2] = x1, o[3] = y1;
    v->boxk[v->nbox++] = b->clean ? 2.0f : CW_CLAMP(b->dim, 0.0f, 1.0f);
  }
}

static int test_int(const char *name) {
  const char *e = cw_test_env(name);
  return e && *e ? atoi(e) : 0;
}

/* the frame's exposure parts and the automatic exposure's state */
static unsigned g_auto_tex;
static float g_exposure;
static unsigned g_depth; /* the depth the looks read (ctx->depth_tex; none in the shared split screen chain) */

static void common(cw_pass_ctx *ctx, unsigned pr, const vinfo *v) {
  cw_pass_bind_texture(0, ctx->src->tex);
  cw_pass_bind_texture(1, g_auto_tex);
  if (!wg()) cw_pass_uniform1i(pr, "u_src", 0), cw_pass_uniform1i(pr, "u_auto", 1);
  cw_pass_uniform4f(pr, "u_rect", (float)v->rect[0], (float)v->rect[1], (float)v->rect[2], (float)v->rect[3]);
  /* CW_TEST_LOOK_CRAWL=1 (a gate's control): the dither pattern moves a pixel a frame, as a pattern not anchored to
   * the screen would */
  float crawl = test_int("CW_TEST_LOOK_CRAWL") ? (float)(ctx->frame % 64) : 0.0f;
  cw_pass_uniform4f(pr, "u_ex", g_exposure, crawl, v->scale, (float)ctx->time);
  cw_pass_uniform4f(pr, "u_bn", (float)v->nbox, (float)(ctx->frame % 16777216), (float)g_stage, 0);
  static const char *const B[16] = { "u_b0", "u_b1", "u_b2", "u_b3", "u_b4", "u_b5", "u_b6", "u_b7",
                                     "u_b8", "u_b9", "u_b10", "u_b11", "u_b12", "u_b13", "u_b14", "u_b15" };
  for (int i = 0; i < 16; i++) {
    const float *b = v->box[i < v->nbox ? i : 0];
    if (i < v->nbox) cw_pass_uniform4f(pr, B[i], b[0], b[1], b[2], b[3]);
    else cw_pass_uniform4f(pr, B[i], 0, 0, 0, 0);
  }
  static const char *const K[4] = { "u_k0", "u_k1", "u_k2", "u_k3" };
  for (int i = 0; i < 4; i++) cw_pass_uniform4f(pr, K[i], v->boxk[i * 4], v->boxk[i * 4 + 1], v->boxk[i * 4 + 2], v->boxk[i * 4 + 3]);
}

/* intermediate targets of the view's own size: the one-pass split screen's scissor off while they are drawn */
static bool clip_saved;
static void small_begin(void) {
  clip_saved = cw__vclip.on;
  cw__vclip.on = false;
  if (!wg()) glDisable(GL_SCISSOR_TEST);
}
static void small_end(void) { cw__vclip.on = clip_saved; }

/* the look's persistent target for this view (history), or a pooled one */
static const cw_rt *hist_rt(cw_pass_ctx *ctx, const look *l, const char *what, int slot, int w, int h, cw_rt_format f, bool *fresh) {
  char key[48];
  snprintf(key, sizeof key, "look%d.%s.%d", (int)(l - LOOKS), what, slot);
  return cw_pipeline_rt_persistent(ctx->pipeline, key, w, h, f, fresh);
}

/* the frame this (look, slot) drew last (0: never); records this one */
static uint64_t seen_swap(looks_state *s, const look *l, int slot, uint64_t frame) {
  uint32_t li = (uint32_t)(l - LOOKS);
  for (int i = 0; i < s->nseen; i++)
    if (s->seen[i].look == li && s->seen[i].slot == (uint32_t)slot) {
      uint64_t f = s->seen[i].frame;
      s->seen[i].frame = frame + 1;
      return f;
    }
  int k = s->nseen < (int)CW_ARRAY_LEN(s->seen) ? s->nseen++ : (int)(frame % CW_ARRAY_LEN(s->seen));
  s->seen[k].look = li, s->seen[k].slot = (uint32_t)slot, s->seen[k].frame = frame + 1;
  return 0;
}

static void draw_ascii(cw_pass_ctx *ctx, looks_state *s, look *l, const vinfo *v, const cw_rt *out) {
  if (!ascii_prepare(l)) return;
  const cw_look_ascii *a = &l->d.ascii;
  int rows_want = CW_MAX(a->rows, 4);
  int ch = CW_MAX(6, (int)lrintf(v->out_h / (float)rows_want));
  int cw = CW_MAX(3, (int)lrintf((float)ch * l->aspect));
  int cols = (v->rect[2] + cw - 1) / cw, rows = (v->rect[3] + ch - 1) / ch;
  dbg_cells = cols * rows;
  unsigned atlas = ascii_atlas(l, cw, ch);
  unsigned pc = prog(P_ASCII_CELLS), pd = prog(P_ASCII_DRAW);
  if (!pc || !pd || !atlas) return;
  unsigned pr = prog(P_ASCII_REG);
  if (!pr) return;
  cw_rt_format f = cw__caps.float_render ? CW_RT_RGBA16F : CW_RT_RGBA8;
  /* the history, two of each (this frame's and last frame's): the cells, and the measures (three texels a cell:
   * regions 0-3; regions 4-5 and the edge; the colour) */
  const cw_rt *H[2][2];
  bool fresh = false;
  for (int k = 0; k < 2; k++)
    for (int j = 0; j < 2; j++) {
      bool fr = false;
      char key[16];
      snprintf(key, sizeof key, "%s%d", k ? "meas" : "cells", j);
      H[k][j] = hist_rt(ctx, l, key, v->slot, k ? cols * 3 : cols, rows, f, &fr);
      if (!H[k][j]) return;
      fresh |= fr;
    }
  int ci = (ctx->frame & 1) ? 1 : 0, pi = ci ^ 1;
  const cw_rt *cur = H[0][ci], *prev = H[0][pi];
  uint64_t last = seen_swap(s, l, v->slot, ctx->frame);
  bool have_prev = !fresh && last == ctx->frame && ctx->frame > 0 && a->hold > 1.0f; /* (last = previous frame + 1) */
  float hold = CW_MAX(a->hold, 1.0f);
  float lin[3] = { 0, 0, 0 };
  if (g_depth) cw_pass_linear_depth_params(ctx, lin);
  small_begin();
  /* the measures */
  cw_pass_bind_target(ctx, H[1][ci]);
  cw_pass_use(pr);
  common(ctx, pr, v);
  cw_pass_bind_texture(2, g_depth ? g_depth : ctx->src->tex);
  cw_pass_bind_texture(3, H[1][pi]->tex);
  cw_pass_bind_texture(4, H[1][pi]->tex);
  cw_pass_bind_texture(5, H[1][pi]->tex);
  if (!wg())
    cw_pass_uniform1i(pr, "u_depth", 2), cw_pass_uniform1i(pr, "u_pr", 3), cw_pass_uniform1i(pr, "u_ps", 4), cw_pass_uniform1i(pr, "u_pc", 5);
  cw_pass_uniform4f(pr, "u_cell", (float)cw, (float)ch, (float)cols, (float)rows);
  cw_pass_uniform4f(pr, "u_lin", g_depth ? lin[0] : -1.0f, lin[1], lin[2], 0);
  cw_pass_uniform4f(pr, "u_par", a->floor, a->contrast, a->edges, hold);
  cw_pass_uniform4f(pr, "u_col", a->exposure, a->saturation, (float)NGLYPH, a->bright_floor);
  cw_pass_uniform4f(pr, "u_ex2", a->flat_bright, a->dim_bright, a->peak, 0);
  /* the measures' hold: on with a history; moves under 0.25 (hold - 1) in brightness and half (hold - 1) in colour
   * ease in by a quarter a frame */
  cw_pass_uniform4f(pr, "u_hold", have_prev ? 1.0f : 0.0f, 0.25f * (hold - 1.0f), 0.5f * (hold - 1.0f), 0.25f);
  cw_pass_fullscreen();
  /* the glyphs */
  cw_pass_bind_target(ctx, cur);
  cw_pass_use(pc);
  common(ctx, pc, v);
  cw_pass_bind_texture(2, H[1][ci]->tex);
  cw_pass_bind_texture(3, H[1][ci]->tex);
  cw_pass_bind_texture(4, H[1][ci]->tex);
  cw_pass_bind_texture(5, l->gv_tex);
  cw_pass_bind_texture(6, prev->tex);
  if (!wg())
    cw_pass_uniform1i(pc, "u_r", 2), cw_pass_uniform1i(pc, "u_s", 3), cw_pass_uniform1i(pc, "u_c", 4), cw_pass_uniform1i(pc, "u_gv", 5),
        cw_pass_uniform1i(pc, "u_prev", 6);
  cw_pass_uniform4f(pc, "u_cell", (float)cw, (float)ch, (float)cols, (float)rows);
  cw_pass_uniform4f(pc, "u_lin", 0, 0, 0, have_prev ? 1.0f : 0.0f);
  cw_pass_uniform4f(pc, "u_par", a->floor, a->contrast, a->edges, hold);
  cw_pass_uniform4f(pc, "u_col", a->exposure, a->saturation, (float)NGLYPH, a->bright_floor);
  /* CW_TEST_LOOK_ASCII_ANY=1 (a gate's control): shape matching may pick any glyph, not only the look's set */
  cw_pass_uniform4f(pc, "u_ext", test_int("CW_TEST_LOOK_ASCII_ANY") ? 1.0f : 0.0f, a->flat, (float)l->ramp_n, a->ramp_gain);
  cw_pass_uniform4f(pc, "u_ex2", a->flat_bright, a->dim_bright, a->peak, 0);
  cw_pass_uniform4f(pc, "u_tint", a->tint[0], a->tint[1], a->tint[2], 0);
  cw_pass_fullscreen();
  /* the glow at the cells' size */
  unsigned pg = prog(P_ASCII_GLOW);
  const cw_rt *glow = pg ? cw_pass_rt(ctx, cols, rows, f, false) : NULL;
  if (glow) {
    cw_pass_bind_target(ctx, glow);
    cw_pass_use(pg);
    common(ctx, pg, v);
    cw_pass_bind_texture(2, cur->tex);
    if (!wg()) cw_pass_uniform1i(pg, "u_cells", 2);
    cw_pass_uniform4f(pg, "u_cell", (float)cw, (float)ch, (float)cols, (float)rows);
    cw_pass_uniform4f(pg, "u_glow", a->glow, a->glow_from, 0, 0);
    cw_pass_fullscreen();
  }
  small_end();
  if (!glow) return;
  cw_pass_bind_target(ctx, out);
  cw_pass_use(pd);
  common(ctx, pd, v);
  cw_pass_bind_texture(2, cur->tex);
  cw_pass_bind_texture(3, atlas);
  cw_pass_bind_texture(4, glow->tex);
  if (!wg()) cw_pass_uniform1i(pd, "u_cells", 2), cw_pass_uniform1i(pd, "u_atlas", 3), cw_pass_uniform1i(pd, "u_gl", 4);
  cw_pass_uniform4f(pd, "u_cell", (float)cw, (float)ch, (float)cols, (float)rows);
  /* CW_TEST_LOOK_ASCII_IDS=1 (a gate's probe): each cell's glyph index as its colour */
  cw_pass_uniform4f(pd, "u_glow", a->glow, a->glow_from, a->neon, test_int("CW_TEST_LOOK_ASCII_IDS") ? 1.0f : 0.0f);
  cw_pass_fullscreen();
  cw_pass_rt_release(ctx, glow);
}

static void draw_halfblock(cw_pass_ctx *ctx, looks_state *s, look *l, const vinfo *v, const cw_rt *out) {
  const cw_look_halfblock *h = &l->d.halfblock;
  int ch = CW_MAX(2, (int)lrintf(v->out_h / (float)CW_MAX(h->rows, 2)));
  int half = CW_MAX(1, ch / 2);
  int cols = (v->rect[2] + half - 1) / half, prow = (v->rect[3] + half - 1) / half;
  dbg_cells = cols * ((prow + 1) / 2);
  unsigned pal = palette_tex(l);
  unsigned pp = prog(P_HB_PX), pd = prog(P_HB_DRAW);
  if (!pp || !pd) return;
  /* quantised (a palette or levels): this frame's pixels and last frame's, for the hold; otherwise one target */
  bool quant = h->palette_count > 0 || h->levels > 1;
  cw_rt_format f = cw__caps.float_render ? CW_RT_RGBA16F : CW_RT_RGBA8;
  const cw_rt *px, *prev = NULL;
  bool have_prev = false;
  if (quant) {
    bool f0 = false, f1 = false;
    const cw_rt *H[2] = { hist_rt(ctx, l, "hb0", v->slot, cols, prow, f, &f0), hist_rt(ctx, l, "hb1", v->slot, cols, prow, f, &f1) };
    if (!H[0] || !H[1]) return;
    int ci = (ctx->frame & 1) ? 1 : 0;
    px = H[ci], prev = H[ci ^ 1];
    uint64_t last = seen_swap(s, l, v->slot, ctx->frame);
    /* CW_TEST_HALFBLOCK_NO_HOLD=1 (a gate's control): no hold */
    have_prev = !f0 && !f1 && last == ctx->frame && ctx->frame > 0 && !test_int("CW_TEST_HALFBLOCK_NO_HOLD");
  } else if (!(px = cw_pass_rt(ctx, cols, prow, f, false))) return;
  small_begin();
  cw_pass_bind_target(ctx, px);
  cw_pass_use(pp);
  common(ctx, pp, v);
  cw_pass_bind_texture(2, pal);
  cw_pass_bind_texture(3, prev ? prev->tex : pal);
  if (!wg()) cw_pass_uniform1i(pp, "u_pal", 2), cw_pass_uniform1i(pp, "u_prev", 3);
  cw_pass_uniform4f(pp, "u_hb", (float)half, (float)CW_CLAMP(h->levels, 0, 256), (float)h->palette_count, 0);
  /* the hold: last frame's palette colour while within 0.03 display steps of the nearest; a level while within 0.15
   * of a step past its middle */
  cw_pass_uniform4f(pp, "u_hp", have_prev ? 1.0f : 0.0f, 0.03f, 0.15f, 0);
  cw_pass_fullscreen();
  small_end();
  cw_pass_bind_target(ctx, out);
  cw_pass_use(pd);
  common(ctx, pd, v);
  cw_pass_bind_texture(2, px->tex);
  if (!wg()) cw_pass_uniform1i(pd, "u_cells", 2);
  cw_pass_uniform4f(pd, "u_hb", (float)half, 0, 0, 0);
  cw_pass_fullscreen();
  if (!quant) cw_pass_rt_release(ctx, px);
}

static void dither_uniforms(unsigned pr, const cw_look_dither *d, int n, bool blue) {
  cw_pass_uniform4f(pr, "u_d", (float)n, d->contrast, d->bias, blue ? 1.0f : 0.0f);
  cw_pass_uniform4f(pr, "u_dark", d->dark[0], d->dark[1], d->dark[2], 0);
  cw_pass_uniform4f(pr, "u_light", d->light[0], d->light[1], d->light[2], 0);
}

/* a setting at 0 is its default */
static float or_def(float v, float def) { return v == 0.0f || v != v ? def : v; }
/* any tone option on: the tone programs (otherwise the plain ones, the frame as before the options) */
static bool dither_tone_on(const cw_look_dither *d) {
  return d->hue_value > 0 || or_def(d->grey_gain, 1) != 1 || d->sharpen > 0 || d->outline > 0 || or_def(d->gamma, 1) != 1;
}
/* the tone programs' own uniforms and the depth (unit 3) */
static void dither_tone_uniforms(cw_pass_ctx *ctx, unsigned pr, const cw_look_dither *d) {
  float lin[3] = { 0, 0, 0 };
  if (g_depth) cw_pass_linear_depth_params(ctx, lin);
  cw_pass_bind_texture(3, g_depth ? g_depth : ctx->src->tex);
  if (!wg()) cw_pass_uniform1i(pr, "u_depth", 3);
  float hf = or_def(d->hue_from, 0.22f), ht = or_def(d->hue_to, 0.5f), kf = or_def(d->keep_from, 0.6f), kt = or_def(d->keep_to, 0.95f);
  float of = or_def(d->outline_from, 0.02f), ot = or_def(d->outline_to, 0.08f);
  cw_pass_uniform4f(pr, "u_t0", CW_MAX(d->hue_value, 0.0f), hf, CW_MAX(ht, hf + 1e-4f), or_def(d->grey_gain, 1));
  cw_pass_uniform4f(pr, "u_t1", kf, CW_MAX(kt, kf + 1e-4f), CW_MAX(d->sharpen, 0.0f), (float)CW_CLAMP((int)lrintf(or_def(d->sharpen_radius, 2)), 1, 16));
  cw_pass_uniform4f(pr, "u_t2", CW_MAX(or_def(d->gamma, 1), 0.01f), CW_MAX(d->outline, 0.0f), of, CW_MAX(ot, of + 1e-4f));
  cw_pass_uniform4f(pr, "u_lin", lin[0], lin[1], lin[2], g_depth ? 1.0f : 0.0f);
}

/* a tone program's uniforms and textures (the small programs and the ordered's last: u_ltex on unit 4, u_lp.x the
 * brightness target's margin in dither pixels); a program that reads no depth gets the noise on its unit (WebGPU
 * binds the depth, R32F, only where a program loads it) */
static void dither_set(cw_pass_ctx *ctx, unsigned pr, const cw_look_dither *d, const vinfo *v, int n, bool blue, unsigned noise,
                       unsigned ltex, int margin, bool depth) {
  cw_pass_use(pr);
  common(ctx, pr, v);
  cw_pass_bind_texture(2, noise);
  if (!wg()) cw_pass_uniform1i(pr, "u_noise", 2);
  dither_uniforms(pr, d, n, blue);
  dither_tone_uniforms(ctx, pr, d);
  if (!depth) cw_pass_bind_texture(3, noise);
  cw_pass_bind_texture(4, ltex);
  if (!wg()) cw_pass_uniform1i(pr, "u_ltex", 4);
  cw_pass_uniform4f(pr, "u_lp", (float)margin, 0, 0, 0);
}

/* the tone options' brightness of every dither pixel the unsharp mask reaches (the view's bw x bh and the mask's
 * radius more on each side), into a small target (released by the caller); NULL when it could not be made */
static const cw_rt *dither_brightness(cw_pass_ctx *ctx, const cw_look_dither *d, const vinfo *v, int n, int bw, int bh,
                                      unsigned noise, int *margin) {
  unsigned pl = prog(P_DITH_SL);
  *margin = d->sharpen > 0 ? CW_CLAMP((int)lrintf(or_def(d->sharpen_radius, 2)), 1, 16) : 0;
  if (test_int("CW_TEST_LOOK_TONE_NO_MARGIN")) *margin = 0; /* (a gate's control: the mask reads past the target) */
  const cw_rt *lt = pl ? cw_pass_rt(ctx, bw + 2 * *margin, bh + 2 * *margin, CW_RT_RGBA8, false) : NULL;
  if (!lt) return NULL;
  cw_pass_bind_target(ctx, lt);
  dither_set(ctx, pl, d, v, n, false, noise, ctx->src->tex, *margin, false);
  cw_pass_fullscreen();
  return lt;
}

static void draw_dither(cw_pass_ctx *ctx, look *l, const vinfo *v, const cw_rt *out) {
  const cw_look_dither *d = &l->d.dither;
  int n = CW_CLAMP((int)lrintf(d->pixel * (d->output_pixels ? 1.0f : v->scale)), 1, 64);
  int bw = (v->rect[2] + n - 1) / n, bh = (v->rect[3] + n - 1) / n;
  unsigned noise = blue_noise_tex();
  bool tone = dither_tone_on(d);
  /* the tone in small targets from 2 x 2 screen pixels a dither pixel up; one a screen pixel repeats no work, and
   * the targets' writes and reads cost more than the eight extra mean colours (0.43 against 0.32 ms at 1080p on a
   * Radeon 890M). CW_TEST_LOOK_TONE_EACH_PIXEL=1 (the gates' before): the tone in every screen pixel at any size. */
  bool each = tone && (n == 1 || test_int("CW_TEST_LOOK_TONE_EACH_PIXEL"));
  dbg_tone = tone ? 1 : 0;
  dbg_tone_small = tone && !each ? 1 : 0;
  if (l->d.kind == CW_LOOK_DITHER_ORDERED) {
    if (tone && !each) {
      unsigned pt = prog(P_DITH_ST), ps = prog(P_DITH_SO);
      if (!pt || !ps) return;
      int margin;
      small_begin();
      const cw_rt *lt = dither_brightness(ctx, d, v, n, bw, bh, noise, &margin);
      const cw_rt *tt = lt ? cw_pass_rt(ctx, bw, bh, CW_RT_RGBA8, false) : NULL;
      if (tt) {
        cw_pass_bind_target(ctx, tt);
        dither_set(ctx, pt, d, v, n, false, noise, lt->tex, margin, true);
        cw_pass_fullscreen();
      }
      small_end();
      if (tt) {
        cw_pass_bind_target(ctx, out);
        dither_set(ctx, ps, d, v, n, d->pattern == CW_DITHER_BLUE_NOISE, noise, tt->tex, margin, false);
        cw_pass_fullscreen();
        cw_pass_rt_release(ctx, tt);
      }
      if (lt) cw_pass_rt_release(ctx, lt);
      return;
    }
    unsigned po = prog(tone ? P_DITH_ORD_T : P_DITH_ORD);
    if (!po) return;
    cw_pass_bind_target(ctx, out);
    cw_pass_use(po);
    common(ctx, po, v);
    cw_pass_bind_texture(2, noise);
    if (!wg()) cw_pass_uniform1i(po, "u_noise", 2);
    dither_uniforms(po, d, n, d->pattern == CW_DITHER_BLUE_NOISE);
    if (tone) dither_tone_uniforms(ctx, po, d);
    cw_pass_fullscreen();
    return;
  }
  unsigned pi = prog(!tone ? P_DITH_INIT : each ? P_DITH_INIT_T : P_DITH_SI), pb = prog(P_DITH_DBS), pd = prog(P_DITH_DRAW);
  if (!pi || !pb || !pd) return;
  const cw_rt *st[2] = { cw_pass_rt(ctx, bw, bh, CW_RT_RGBA8, false), cw_pass_rt(ctx, bw, bh, CW_RT_RGBA8, false) };
  if (!st[0] || !st[1]) return;
  small_begin();
  const cw_rt *lt = NULL;
  int margin = 0;
  if (tone && !each && !(lt = dither_brightness(ctx, d, v, n, bw, bh, noise, &margin))) {
    small_end();
    cw_pass_rt_release(ctx, st[0]);
    cw_pass_rt_release(ctx, st[1]);
    return;
  }
  cw_pass_bind_target(ctx, st[0]);
  if (lt) dither_set(ctx, pi, d, v, n, true, noise, lt->tex, margin, true);
  else {
    cw_pass_use(pi);
    common(ctx, pi, v);
    cw_pass_bind_texture(2, noise);
    if (!wg()) cw_pass_uniform1i(pi, "u_noise", 2);
    dither_uniforms(pi, d, n, true);
    if (tone) dither_tone_uniforms(ctx, pi, d);
  }
  cw_pass_fullscreen();
  if (lt) cw_pass_rt_release(ctx, lt);
  int cur = 0, sweeps = CW_CLAMP(d->sweeps, 0, 8);
  for (int sw = 0; sw < sweeps; sw++)
    for (int ph = 0; ph < 4; ph++) {
      cw_pass_bind_target(ctx, st[cur ^ 1]);
      cw_pass_use(pb);
      common(ctx, pb, v);
      cw_pass_bind_texture(2, st[cur]->tex);
      if (!wg()) cw_pass_uniform1i(pb, "u_state", 2);
      static const int PX[4] = { 0, 1, 1, 0 }, PY[4] = { 0, 1, 0, 1 };
      cw_pass_uniform4f(pb, "u_s", (float)PX[ph], (float)PY[ph], 0, 0);
      cw_pass_fullscreen();
      cur ^= 1;
    }
  small_end();
  cw_pass_bind_target(ctx, out);
  cw_pass_use(pd);
  common(ctx, pd, v);
  cw_pass_bind_texture(2, st[cur]->tex);
  if (!wg()) cw_pass_uniform1i(pd, "u_state", 2);
  dither_uniforms(pd, d, n, false);
  cw_pass_fullscreen();
  cw_pass_rt_release(ctx, st[0]);
  cw_pass_rt_release(ctx, st[1]);
}

static void draw_custom(cw_pass_ctx *ctx, look *l, const vinfo *v, const cw_rt *out) {
  unsigned *pr = &l->prog[g_stage];
  if (!*pr || l->prog_ver[g_stage] != l->ver) {
    char name[48], body[32768];
    snprintf(name, sizeof name, "look.custom.%d.%d", (int)(l - LOOKS), l->ver);
    if (wg()) snprintf(body, sizeof body, "%s%s%s", CUSTOM_W_HELP, l->wgsl ? l->wgsl : "", CUSTOM_W_MAIN);
    else snprintf(body, sizeof body, "%s%s%s", CUSTOM_G_HELP, l->glsl ? l->glsl : "", CUSTOM_G_MAIN);
    *pr = (wg() ? l->wgsl : l->glsl) ? build(name, CUSTOM_G_DECL, body, CUSTOM_W_FIELDS, CUSTOM_W_DECL, body) : 0;
    l->prog_ver[g_stage] = l->ver;
    if (!*pr) cw_error("cw_look: the CUSTOM look's shader did not build: it draws the plain scene");
  }
  if (!*pr) {
    cw__pass_copy(ctx, ctx->src->tex, out);
    return;
  }
  unsigned noise = blue_noise_tex();
  float lin[3] = { 0, 0, 0 };
  if (g_depth) cw_pass_linear_depth_params(ctx, lin);
  cw_pass_bind_target(ctx, out);
  cw_pass_use(*pr);
  common(ctx, *pr, v);
  cw_pass_bind_texture(2, g_depth ? g_depth : ctx->src->tex);
  cw_pass_bind_texture(3, noise);
  if (!wg()) cw_pass_uniform1i(*pr, "u_depth", 2), cw_pass_uniform1i(*pr, "u_noise", 3);
  const float *p = l->d.custom.params;
  cw_pass_uniform4f(*pr, "u_p0", p[0], p[1], p[2], p[3]);
  cw_pass_uniform4f(*pr, "u_p1", p[4], p[5], p[6], p[7]);
  cw_pass_uniform4f(*pr, "u_p2", p[8], p[9], p[10], p[11]);
  cw_pass_uniform4f(*pr, "u_p3", p[12], p[13], p[14], p[15]);
  cw_pass_uniform4f(*pr, "u_lin", lin[0], lin[1], lin[2], g_depth ? 1.0f : 0.0f);
  cw_pass_fullscreen();
}

static void draw_look(cw_pass_ctx *ctx, looks_state *s, look *l, const vinfo *v, const cw_rt *out) {
  switch (l->d.kind) {
  case CW_LOOK_ASCII: draw_ascii(ctx, s, l, v, out); break;
  case CW_LOOK_HALFBLOCK: draw_halfblock(ctx, s, l, v, out); break;
  case CW_LOOK_DITHER_ORDERED:
  case CW_LOOK_DITHER_DIFFUSE: draw_dither(ctx, l, v, out); break;
  case CW_LOOK_CUSTOM: draw_custom(ctx, l, v, out); break;
  default: break;
  }
}

/* the mean exposed luminance of img in the view, reduced into the 1x1 RGBA8 target "look.lum<which>" (packed) */
static void measure_into(cw_pass_ctx *ctx, const vinfo *v, const cw_rt *img, int which) {
  unsigned pl = prog(P_LUM);
  if (!pl) return;
  int w = v->rect[2], h = v->rect[3];
  const cw_rt *cur = img;
  int lo[2] = { v->rect[0], v->rect[1] }, hi[2] = { v->rect[0] + w, v->rect[1] + h };
  bool first = true;
  char key[24];
  snprintf(key, sizeof key, "look.lum%d", which);
  small_begin();
  while (true) {
    int nw = CW_MAX(1, (w + 7) / 8), nh = CW_MAX(1, (h + 7) / 8);
    bool last = nw == 1 && nh == 1;
    const cw_rt *rt = last ? cw_pipeline_rt_persistent(ctx->pipeline, key, 1, 1, CW_RT_RGBA8, NULL)
                           : cw_pass_rt(ctx, nw, nh, cw__caps.float_render ? CW_RT_RGBA16F : CW_RT_RGBA8, false);
    if (!rt) break;
    cw_pass_bind_target(ctx, rt);
    cw_pass_use(pl);
    common(ctx, pl, v);
    cw_pass_bind_texture(2, cur->tex);
    if (!wg()) cw_pass_uniform1i(pl, "u_img", 2);
    cw_pass_uniform4f(pl, "u_lm", (float)hi[0], (float)hi[1], (float)lo[0], (float)lo[1]);
    cw_pass_uniform4f(pl, "u_lf", first ? 1.0f : 0.0f, last ? 1.0f : 0.0f, 0, 0);
    cw_pass_fullscreen();
    if (!first) cw_pass_rt_release(ctx, cur);
    cur = rt;
    first = false;
    w = nw, h = nh, lo[0] = lo[1] = 0, hi[0] = nw, hi[1] = nh;
    if (last) break;
  }
  small_end();
}

/* each drawn look's brightness: reduced in one frame, its 1x1 read asked the next (on GL the read then waits only for
 * the previous frame's work), taken when it lands */
static void measure(cw_pass_ctx *ctx, looks_state *s, const vinfo *v, const cw_rt *img, int which) {
  char key[24];
  snprintf(key, sizeof key, "look.lum%d", which);
  s->drawn |= (uint8_t)(1u << which);
  if (s->pend[which] && !s->ticket[which]) {
    const cw_rt *m = cw__pipeline_rt_find(ctx->pipeline, key);
    s->ticket[which] = m ? cw_gpu_read_rt(m, 0, 0, 1, 1) : 0;
    s->pend[which] = false;
  }
  if (s->ticket[which]) {
    uint8_t px[4];
    int got = cw_gpu_read_result(s->ticket[which], px);
    if (got != 0) {
      if (got > 0) s->lum[which] = (float)(px[0] * 256 + px[1]) / 65535.0f * 64.0f, s->have_lum = true;
      s->ticket[which] = 0;
    }
  }
  if (!s->pend[which] && !s->ticket[which]) {
    measure_into(ctx, v, img, which);
    s->pend[which] = true;
  }
}

static void looks_run(cw_pass_ctx *ctx, looks_state *s);
static void looks_prewarm_run(cw_pass_ctx *ctx, looks_state *s);
/* CW_TEST_PREWARM_SHOW=1: the prewarm drawn after the pass's real run, so it is what dst holds (the control of gate
 * backdrop-look-prewarm's invisible check; getenv: a prewarm runs in a test's first frame, before cw_test_env answers) */
static bool prewarm_show(void) {
  const char *v = getenv("CW_TEST_PREWARM_SHOW");
  return v && atoi(v);
}
static void looks_exec(cw_pass_ctx *ctx, void *u) {
  g_stage = 0;
  bool show = prewarm_show();
  if (!show) looks_prewarm_run(ctx, u);
  looks_run(ctx, u);
  if (show) looks_prewarm_run(ctx, u);
}
static void looks_display_exec(cw_pass_ctx *ctx, void *u) {
  g_stage = 1;
  bool show = prewarm_show();
  if (!show) looks_prewarm_run(ctx, u);
  looks_run(ctx, u);
  if (show) looks_prewarm_run(ctx, u);
  g_stage = 0;
}
static void looks_run(cw_pass_ctx *ctx, looks_state *s) {
  look *la, *lb;
  bool na, nb;
  plan(s, &la, &lb, &na, &nb);
  if (!ctx->src || !ctx->dst) return;
  /* the textures first: making one binds it, which would replace a unit already bound */
  unsigned neutral = neutral_tex();
  if ((la && (la->d.kind == CW_LOOK_DITHER_ORDERED || la->d.kind == CW_LOOK_DITHER_DIFFUSE || la->d.kind == CW_LOOK_CUSTOM)) ||
      (lb && (lb->d.kind == CW_LOOK_DITHER_ORDERED || lb->d.kind == CW_LOOK_DITHER_DIFFUSE || lb->d.kind == CW_LOOK_CUSTOM)))
    blue_noise_tex();
  if (la && la->d.kind == CW_LOOK_ASCII) ascii_prepare(la);
  if (lb && lb->d.kind == CW_LOOK_ASCII) ascii_prepare(lb);
  const cw_post_settings *st = ctx->settings;
  g_auto_tex = st->auto_exposure && ctx->pipeline->auto_exposure_tex ? ctx->pipeline->auto_exposure_tex : neutral;
  g_exposure = st->exposure * exp2f(st->exposure_ev);
  if (g_stage == 1) g_auto_tex = neutral, g_exposure = 1; /* the finished picture: the tone curve took the exposure */
  g_depth = ctx->depth_tex;
  if (g_stage == 1 && ctx->pipeline->views_shared_post && ctx->pipeline->nviews >= 2 && ctx->view_count == 0) g_depth = 0;
  /* a blend of looks of two stages (pair_stages): each stage blends its own look with the plain picture it gets */
  bool cross = la && lb && la != lb && na && nb && la->d.stage != lb->d.stage && !test_int("CW_TEST_LOOK_ONE_STAGE");
  dbg_stage = cross ? 2 : g_stage;
  vinfo v;
  view_info(ctx, s, &v);
  if (v.slot < 8) dbg_views = v.slot == 0 ? 1 : dbg_views + 1; /* (not an extra view: a SubViewport) */
  dbg_kind_a = la ? (int32_t)la->d.kind : 0, dbg_kind_b = lb ? (int32_t)lb->d.kind : 0, dbg_t = s->t;
  if (cross) {
    if ((la->d.stage == CW_LOOK_STAGE_DISPLAY) != (g_stage == 1)) la = NULL;
    else lb = NULL;
  }
  bool want_measure = s->measure && v.first;
  if (want_measure && !(cross && g_stage == 1)) s->drawn = 0; /* (a blend across stages: the SCENE stage ran first) */
  /* a look alone (t 0 or 1, or the other side is the plain scene at that end) straight into dst */
  bool blend = na && nb && la != lb;
  look *only = !blend ? (na ? la : lb) : NULL;
  if (!blend) {
    dbg_ran = only == la ? 1 : 2;
    if (only) draw_look(ctx, s, only, &v, ctx->dst);
    else cw__pass_copy(ctx, ctx->src->tex, ctx->dst);
    if (want_measure) {
      measure(ctx, s, &v, only ? ctx->dst : ctx->src, na ? 0 : 1);
      s->lum[na ? 1 : 0] = -1; /* the other look was not drawn */
    }
    return;
  }
  dbg_ran = 7;
  const cw_rt *ia = la ? cw_pass_rt(ctx, ctx->width, ctx->height, ctx->src->format, false) : ctx->src;
  const cw_rt *ib = lb ? cw_pass_rt(ctx, ctx->width, ctx->height, ctx->src->format, false) : ctx->src;
  if (!ia || !ib) return;
  if (la) draw_look(ctx, s, la, &v, ia);
  if (lb) draw_look(ctx, s, lb, &v, ib);
  if (want_measure) {
    if (la || !cross) measure(ctx, s, &v, ia, 0);
    if (lb || !cross) measure(ctx, s, &v, ib, 1);
  }
  unsigned pm = prog(P_MIX);
  if (pm) {
    cw_pass_bind_target(ctx, ctx->dst);
    cw_pass_use(pm);
    common(ctx, pm, &v);
    cw_pass_bind_texture(2, ia->tex);
    cw_pass_bind_texture(3, ib->tex);
    if (!wg()) cw_pass_uniform1i(pm, "u_a", 2), cw_pass_uniform1i(pm, "u_b", 3);
    const cw_look_mix *h = &s->how;
    float sn, cs;
    cw_sincos(h->angle, &sn, &cs);
    float grain = (h->scale > 0 ? h->scale : 24.0f) * v.scale;
    cw_pass_uniform4f(pm, "u_mix", s->t, (float)CW_CLAMP((int)h->blend, 0, 2), CW_CLAMP(h->softness, 0.0f, 1.0f), grain);
    cw_pass_uniform4f(pm, "u_dir", cs, sn, (float)(h->seed % 65536u), 0);
    cw_pass_fullscreen();
  }
  if (la) cw_pass_rt_release(ctx, ia);
  if (lb) cw_pass_rt_release(ctx, ib);
}

/* ── prewarm ─────────────────────────────────────────────────────────── */

/* The prewarms waiting for this stage, in the pass's first run of the frame: each look drawn twice into this run's
 * own target (dst, which the pass's real run then writes over whole), with the pipeline's real frame (size, formats,
 * depth, views): alone (its last draw into dst's format, the screen's at the DISPLAY stage) and in a blend with the
 * plain scene at weight 0.5 (the look into a pooled target, then the mix into dst). So every program, WebGPU pipeline
 * (keyed on the target's format too), glyph atlas, palette and target its first shown frame needs is made here. What
 * it leaves (a glyph history, the debug fields) is put back. */
static void looks_prewarm_run(cw_pass_ctx *ctx, looks_state *s) {
  if (!s->nwarm || !ctx->src || !ctx->dst || !warm_wants(s, g_stage)) return;
  if (ctx->view_count > 0 ? ctx->view_index != 0 : (ctx->pipeline->nviews >= 2 && ctx->pipeline->view_i > 0)) return; /* (the first view) */
  CW_PROFILE_SCOPE("look.prewarm");
  cw_look ka = s->a, kb = s->b;
  float kt = s->t;
  cw_look_mix kh = s->how;
  bool km = s->measure;
  int knseen = s->nseen;
  __typeof__(s->seen) kseen;
  memcpy(kseen, s->seen, sizeof kseen);
  int32_t d0 = dbg_ran, d1 = dbg_cells, d2 = dbg_views, d3 = dbg_kind_a, d4 = dbg_kind_b, d5 = dbg_stage, d6 = dbg_tone;
  float d7 = dbg_t;
  for (int i = 0; i < s->nwarm; i++) {
    if (!(s->warm[i].stages & (1u << g_stage))) continue;
    s->warm[i].stages &= (uint8_t)~(1u << g_stage);
    if (!drawable(s->warm[i].l)) continue;
    s->how = (cw_look_mix){ 0 }, s->measure = false;
    s->a = s->warm[i].l, s->b = (cw_look){ 0 }, s->t = 0;
    looks_run(ctx, s); /* alone */
    s->t = 0.5f;
    looks_run(ctx, s); /* in a blend */
    s->prewarms++;
  }
  /* the entries done at every stage leave the list */
  int k = 0;
  for (int i = 0; i < s->nwarm; i++)
    if (s->warm[i].stages) s->warm[k++] = s->warm[i];
  s->nwarm = k;
  s->a = ka, s->b = kb, s->t = kt, s->how = kh, s->measure = km;
  s->nseen = knseen;
  memcpy(s->seen, kseen, sizeof kseen);
  dbg_ran = d0, dbg_cells = d1, dbg_views = d2, dbg_kind_a = d3, dbg_kind_b = d4, dbg_stage = d5, dbg_tone = d6, dbg_t = d7;
  /* (a pass that runs only for the prewarm leaves the picture as it was: looks_run copies src to dst with no look) */
}

/* the stage a look is drawn at: its own (each look of a pair is drawn at its own stage, a blend across stages at both;
 * CW_TEST_LOOK_ONE_STAGE, a gate's control, draws the pair at DISPLAY, the SCENE look too) */
static uint8_t look_stages(const look *l) {
  uint8_t st = l->d.stage == CW_LOOK_STAGE_DISPLAY ? 2u : 1u;
  if (test_int("CW_TEST_LOOK_ONE_STAGE")) st |= 2u;
  return st;
}

bool cw_look_prewarm(cw_pipeline *p, cw_look h) {
  look *l = drawable(h);
  if (!p || !l) return false;
  looks_state *s = state_of(p);
  if (!s) return false;
  uint8_t st = look_stages(l);
  for (int i = 0; i < s->nwarm; i++)
    if (s->warm[i].l.id == h.id) {
      s->warm[i].stages |= st;
      return true;
    }
  if (s->nwarm >= (int)CW_ARRAY_LEN(s->warm)) return false;
  s->warm[s->nwarm].l = h, s->warm[s->nwarm].stages = st;
  s->nwarm++;
  return true;
}

int cw_look_prewarm_all(cw_pipeline *p) {
  int k = 0;
  for (int i = 1; i < MAX_LOOKS; i++)
    if (LOOKS[i].live && LOOKS[i].d.kind != CW_LOOK_NONE) k += cw_look_prewarm(p, CW_HANDLE_MAKE(cw_look, i, LOOKS[i].gen));
  return k;
}

int cw_pipeline_look_prewarms(cw_pipeline *p) {
  looks_state *s = p ? state_find(p) : NULL;
  return s ? s->prewarms : 0;
}

/* Graphics settings (cartwheel/quality.h): the settings, the presets that
 * set them, Auto, and what the renderer asks of them (quality_internal.h). */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "cartwheel/quality.h"
#include "cartwheel/debug.h"
#include "cartwheel/core.h"
#include "cartwheel/gpu.h"
#include "cartwheel/platform.h"
#include "cartwheel/pipeline.h"
#include "cartwheel/profile.h"
#include "cartwheel/scene.h"
#include "pipeline_internal.h"
#include "quality_internal.h"

/* the platform (src/platform): the scene's share of the output, and the output's size */
void cw__render_scale_apply(float s);
void cw__output_size(int *w, int *h);
void cw__quality_menu_register(void); /* src/shell/quality_menu.c: its debug command */

/* ── the settings and the presets ──────────────────────────────────── */

static const float RES[] = { 0.5f, 0.6f, 2.0f / 3.0f, 0.75f, 0.85f, 1.0f };
enum { RES_N = 6, RES_100 = 5, RES_85 = 4, RES_75 = 3, RES_67 = 2, RES_60 = 1, RES_50 = 0 };
enum { SH_LOW, SH_MEDIUM, SH_HIGH, SH_ULTRA, SH_N };
enum { AA_OFF, AA_FXAA, AA_SMAA, AA_N };
static const int VALUES[CW_GFX_ENGINE_COUNT] = { RES_N, 2, SH_N, AA_N, CW_QUALITY_COUNT };
static const char *const SETTING_NAMES[CW_GFX_ENGINE_COUNT] = { "Resolution", "Ambient occlusion", "Shadows", "Anti-aliasing",
                                                                "Effects detail" };
static const char *const NAMES[CW_QUALITY_COUNT] = { "Low", "Medium", "High", "Ultra" };
static const char *const ONOFF[2] = { "Off", "On" };
static const char *const SHADOWS[SH_N] = { "Low", "Medium", "High", "Ultra" };
static const char *const AAS[AA_N] = { "Off", "FXAA", "SMAA" };

/* each preset's settings: resolution, ssao, shadows, aa, effects */
static const uint8_t PRESET[CW_QUALITY_COUNT][CW_GFX_ENGINE_COUNT] = {
  [CW_QUALITY_LOW] = { RES_67, 0, SH_LOW, AA_FXAA, CW_QUALITY_LOW },
  [CW_QUALITY_MEDIUM] = { RES_75, 1, SH_MEDIUM, AA_FXAA, CW_QUALITY_MEDIUM },
  [CW_QUALITY_HIGH] = { RES_100, 1, SH_HIGH, AA_SMAA, CW_QUALITY_HIGH },
  [CW_QUALITY_ULTRA] = { RES_100, 1, SH_ULTRA, AA_SMAA, CW_QUALITY_ULTRA },
};

/* effects detail */
typedef struct {
  float particles, density, lod_bias;
  float hrtf; /* share of the HRTF voice limit (cartwheel/audio.h) */
  int bloom_levels; /* cap (0 = as authored) */
  int ssao;         /* 1 one slice of 4 steps, 2 as authored, 3 raised */
  float lights;     /* clustered lights a cluster keeps (cartwheel/lighting.h max_per_cluster) */
  float distortion; /* share of the frame size the distortion buffer is drawn at (heat haze, shockwaves) */
  float dispersion; /* distortion: colour fringing at the edges (0 = off) */
  float flash_lights; /* share of the explosion flash light pool used */
  float skin;         /* skin shading (cartwheel/skin_shading.h): 0 = plain PBR */
} effects;
static const effects FX[CW_QUALITY_COUNT] = {
  [CW_QUALITY_LOW] = { 0.4f, 0.35f, 2.0f, 0.25f, 3, 1, 0.375f, 0.5f, 0, 0.25f, 0 },
  [CW_QUALITY_MEDIUM] = { 0.7f, 0.65f, 1.5f, 0.5f, 5, 1, 0.5f, 0.5f, 0, 0.5f, 1 },
  [CW_QUALITY_HIGH] = { 1, 1, 1, 1, 0, 2, 1, 1, 1, 1, 1 },
  [CW_QUALITY_ULTRA] = { 1, 1, 1, 1, 0, 3, 1, 1, 1, 1, 1 },
};

/* cw_quality_full_resolution(false): Auto's ladder with resolution steps, top first: a preset, its resolution replaced */
typedef struct { uint8_t preset, res; } rung_t;
static const rung_t LADDER[] = {
  { CW_QUALITY_HIGH, RES_100 }, { CW_QUALITY_HIGH, RES_85 }, { CW_QUALITY_MEDIUM, RES_75 }, { CW_QUALITY_MEDIUM, RES_67 },
  { CW_QUALITY_LOW, RES_67 },   { CW_QUALITY_LOW, RES_60 },  { CW_QUALITY_LOW, RES_50 },
};
enum { LADDER_N_ALL = (int)(sizeof LADDER / sizeof LADDER[0]) };
/* the default: the ladder without resolution steps (each preset at 100%) */
static const rung_t LADDER_FULL[] = {
  { CW_QUALITY_HIGH, RES_100 }, { CW_QUALITY_MEDIUM, RES_100 }, { CW_QUALITY_LOW, RES_100 },
};
static bool full_res = true;
#define LADDER_N (full_res ? (int)(sizeof LADDER_FULL / sizeof LADDER_FULL[0]) : LADDER_N_ALL)
#define RUNG(r) (full_res ? LADDER_FULL[r] : LADDER[r])

typedef struct {
  const char *name;
  const char *const *values;
  int count;
  uint8_t preset[CW_QUALITY_COUNT];
} lever;

enum { MODE_PRESET, MODE_CUSTOM, MODE_AUTO };

static struct {
  bool inited;
  int32_t level;   /* cw.quality: the preset (Custom: the one it came from) */
  int32_t mode;    /* cw.quality_mode */
  uint8_t v[CW_GFX_ENGINE_COUNT + CW_GFX_MAX_LEVERS];
  lever levers[CW_GFX_MAX_LEVERS];
  int n_levers;
  bool chosen;     /* the player (or a save) chose: the game's Auto default no longer applies */
  /* Auto */
  int rung, fail_rung; /* the ladder's level now; the highest level over budget in this run (-1 none) */
  float budget;
  bool in_play, gpu_timed;
  bool retrying;   /* a run trying the level above its saved one: unproven until it has held for a while */
  double last_t;
  float avg;
  int frames;
  cw_quality_change_fn fn;
  void *fn_user;
} Q = { .level = CW_QUALITY_HIGH, .budget = 19, .fail_rung = -1,
        .v = { RES_100, 1, SH_HIGH, AA_SMAA, CW_QUALITY_HIGH } };

/* scopes (cw_quality_scope_push): caps on top of the player's settings, innermost last */
enum { SCOPE_MAX = 4 };
static struct { char name[24]; int8_t cap[CW_GFX_ENGINE_COUNT]; } SCOPES[SCOPE_MAX];
static int n_scopes;
/* a setting as drawn: the player's value, lowered by every scope that caps it */
static int ev(int s) {
  int v = Q.v[s];
  for (int i = 0; i < n_scopes; i++)
    if (SCOPES[i].cap[s] >= 0 && v > SCOPES[i].cap[s]) v = SCOPES[i].cap[s];
  return v;
}

static void cmd_quality(int argc, const char **argv, cw_reply *r);
static bool forced; /* CW_QUALITY (native): this preset whatever else asks */

static void init_once(void) {
  if (Q.inited) return;
  Q.inited = true;
  cw_debug_field("cw.quality", &Q.level, CW_DBG_I32, 1);
  cw_debug_field("cw.quality_mode", &Q.mode, CW_DBG_I32, 1);
  cw_debug_field("cw.quality_scopes", &n_scopes, CW_DBG_I32, 1);
  cw_debug_command("quality",
                   "quality [low|medium|high|ultra|auto|lock | set <setting> <value> | scope push <setting>=<cap>... | scope pop]: the graphics settings (cartwheel/quality.h); "
                   "settings: resolution 0-5, ssao 0-1, shadows 0-3, aa 0-2, effects 0-3, or a game setting's name (spaces as _)",
                   cmd_quality);
  cw__quality_menu_register();
}

/* Auto, and a saved choice, never apply in a deterministic run (every host
 * starts at High); a choice made in it does (it comes from input every host
 * replays alike). CW_QUALITY fixes the preset against everything. */
static bool test_lock; /* cw__quality_lock: a test host that is not a deterministic run */
static bool locked(void) { return forced || test_lock || cw_is_deterministic_run(); }

/* Auto and a saved choice locked out as in a deterministic run, for a test host (Chromium, wasmcart-native) that keeps its
 * real clock: Auto steps by how fast this machine ran, and a test's picture must not depend on that */
void cw__quality_lock(void) {
  init_once();
  test_lock = true;
  if (Q.mode == MODE_AUTO) {
    Q.mode = MODE_PRESET;
    cw_quality_set(CW_QUALITY_HIGH, false);
  }
  cw_info("quality: locked for a test run");
}

/* the settings took effect: the platform's scene size, the game's callback */
static void changed(void) {
  cw__render_scale_apply(RES[ev(CW_GFX_RESOLUTION)]);
  Q.frames = 0;
  Q.avg = 0;
  if (Q.fn) Q.fn((cw_quality)Q.level, Q.fn_user);
}

static void apply_preset(cw_quality q, int res) {
  for (int i = 0; i < CW_GFX_ENGINE_COUNT; i++) Q.v[i] = PRESET[q][i];
  if (res >= 0) Q.v[CW_GFX_RESOLUTION] = (uint8_t)res;
  if (full_res) Q.v[CW_GFX_RESOLUTION] = RES_100; /* the game never draws below its output size */
  for (int i = 0; i < Q.n_levers; i++) Q.v[CW_GFX_ENGINE_COUNT + i] = Q.levers[i].preset[q];
  Q.level = q;
}

/* the preset every setting matches, else -1 */
static int matching_preset(void) {
  for (int q = 0; q < CW_QUALITY_COUNT; q++) {
    bool same = true;
    for (int i = 0; i < CW_GFX_ENGINE_COUNT && same; i++)
      same = Q.v[i] == (full_res && i == CW_GFX_RESOLUTION ? RES_100 : PRESET[q][i]);
    for (int i = 0; i < Q.n_levers && same; i++) same = Q.v[CW_GFX_ENGINE_COUNT + i] == Q.levers[i].preset[q];
    if (same) return q;
  }
  return -1;
}

static void set_rung(int rung) {
  Q.rung = CW_CLAMP(rung, 0, LADDER_N - 1);
  apply_preset((cw_quality)RUNG(Q.rung).preset, RUNG(Q.rung).res);
}

void cw__quality_init(void) {
  init_once();
  if (cw_is_deterministic_run()) { /* every host draws the same frame */
    apply_preset(CW_QUALITY_HIGH, -1);
    Q.mode = MODE_PRESET;
  }
#if !CW_CART
  /* CW_QUALITY=low|medium|high|ultra: a fixed preset (comparing presets in a test run) */
  const char *env = getenv("CW_QUALITY");
  for (int i = 0; env && *env && i < CW_QUALITY_COUNT; i++)
    if (!strcasecmp(env, NAMES[i])) {
      apply_preset((cw_quality)i, -1);
      Q.mode = MODE_PRESET;
      forced = true;
    }
#endif
  cw__render_scale_apply(RES[ev(CW_GFX_RESOLUTION)]);
}

cw_quality cw_quality_get(void) { return (cw_quality)Q.level; }
const char *cw_quality_name(cw_quality q) { return q >= 0 && q < CW_QUALITY_COUNT ? NAMES[q] : "?"; }

void cw_quality_set(cw_quality q, bool user) {
  init_once();
  if (forced || q < 0 || q >= CW_QUALITY_COUNT) return;
  if (user) {
    Q.chosen = true;
    Q.mode = MODE_PRESET;
  }
  if (Q.mode == MODE_AUTO) { /* the game or a tool: Auto goes on from the nearest level */
    int r = 0;
    while (r < LADDER_N - 1 && RUNG(r).preset > q) r++;
    set_rung(r);
  } else {
    Q.mode = MODE_PRESET;
    apply_preset(q, -1);
  }
  changed();
}

/* ── the settings ──────────────────────────────────────────────────── */

static bool valid(int s) { return s >= 0 && s < CW_GFX_ENGINE_COUNT + Q.n_levers; }
int cw_gfx_count(void) { return CW_GFX_ENGINE_COUNT + Q.n_levers; }
const char *cw_gfx_name(int s) {
  if (!valid(s)) return "?";
  return s < CW_GFX_ENGINE_COUNT ? SETTING_NAMES[s] : Q.levers[s - CW_GFX_ENGINE_COUNT].name;
}
int cw_gfx_value_count(int s) {
  if (!valid(s)) return 0;
  return s < CW_GFX_ENGINE_COUNT ? VALUES[s] : Q.levers[s - CW_GFX_ENGINE_COUNT].count;
}
int cw_gfx_get(int s) { return valid(s) ? Q.v[s] : 0; }
float cw_gfx_render_scale(void) { return RES[ev(CW_GFX_RESOLUTION)]; }
int cw_gfx_effective(int s) { return valid(s) ? (s < CW_GFX_ENGINE_COUNT ? ev(s) : Q.v[s]) : 0; }

const char *cw_gfx_value_label(int s) {
  static char buf[48];
  if (!valid(s)) return "?";
  int v = Q.v[s];
  switch (s) {
  case CW_GFX_RESOLUTION: {
    int w = 0, h = 0;
    cw__output_size(&w, &h);
    float k = RES[v];
    if (w > 0 && h > 0)
      snprintf(buf, sizeof buf, "%d%%  %dx%d", (int)(k * 100 + 0.5f), CW_MAX(1, (int)(w * k + 0.5f)), CW_MAX(1, (int)(h * k + 0.5f)));
    else snprintf(buf, sizeof buf, "%d%%", (int)(k * 100 + 0.5f));
    return buf;
  }
  case CW_GFX_SSAO: return ONOFF[v];
  case CW_GFX_SHADOWS: return SHADOWS[v];
  case CW_GFX_AA: return AAS[v];
  case CW_GFX_EFFECTS: return NAMES[v];
  default: return Q.levers[s - CW_GFX_ENGINE_COUNT].values[v];
  }
}

void cw_gfx_set(int s, int value) {
  init_once();
  if (forced || !valid(s)) return;
  value = CW_CLAMP(value, 0, cw_gfx_value_count(s) - 1);
  Q.chosen = true;
  Q.v[s] = (uint8_t)value;
  int p = matching_preset();
  if (p >= 0) {
    Q.mode = MODE_PRESET;
    Q.level = p;
  } else Q.mode = MODE_CUSTOM;
  changed();
}

void cw_gfx_step(int s, int dir) {
  int n = cw_gfx_value_count(s);
  if (n <= 0) return;
  cw_gfx_set(s, ((cw_gfx_get(s) + (dir < 0 ? -1 : 1)) % n + n) % n);
}

int cw_quality_lever_add(const char *name, const char *const *values, int value_count, const uint8_t preset[CW_QUALITY_COUNT]) {
  init_once();
  if (Q.n_levers >= CW_GFX_MAX_LEVERS || !name || !values || value_count < 1) return -1;
  lever *l = &Q.levers[Q.n_levers];
  l->name = name;
  l->values = values;
  l->count = CW_MIN(value_count, CW_GFX_LEVER_MAX_VALUES);
  for (int q = 0; q < CW_QUALITY_COUNT; q++) l->preset[q] = (uint8_t)CW_CLAMP(preset ? preset[q] : l->count - 1, 0, l->count - 1);
  int id = CW_GFX_ENGINE_COUNT + Q.n_levers++;
  Q.v[id] = l->preset[Q.level]; /* what the preset now picks */
  return id;
}

/* ── reading them ──────────────────────────────────────────────────── */

static const effects *fx(void) { return &FX[ev(CW_GFX_EFFECTS)]; }
float cw_quality_scale(const char *knob) {
  if (!knob) return 1;
  if (!strcmp(knob, "particles")) return fx()->particles;
  if (!strcmp(knob, "lights")) return fx()->lights;
  if (!strcmp(knob, "density") || !strcmp(knob, "grass") || !strcmp(knob, "crowd") || !strcmp(knob, "scatter")) return fx()->density;
  if (!strcmp(knob, "hrtf_voices")) return fx()->hrtf;
  if (!strcmp(knob, "distortion")) return fx()->distortion;
  if (!strcmp(knob, "dispersion")) return fx()->dispersion;
  if (!strcmp(knob, "flash_lights")) return fx()->flash_lights;
  if (!strcmp(knob, "skin")) return fx()->skin;
  return 1;
}
float cw_quality_density(void) { return fx()->density; }
int cw_quality_count(int n, const char *knob) {
  if (n <= 0) return n;
  int k = (int)((float)n * cw_quality_scale(knob) + 0.5f);
  return k < 1 ? 1 : k > n ? n : k;
}

void cw_quality_on_change(cw_quality_change_fn fn, void *user) {
  Q.fn = fn;
  Q.fn_user = user;
}

/* ── Auto ──────────────────────────────────────────────────────────── */

static void auto_start(int rung) {
  Q.mode = MODE_AUTO;
  Q.fail_rung = -1;
  Q.retrying = false;
  set_rung(rung);
  changed();
}

void cw_quality_full_resolution(bool on) {
  init_once();
  if (full_res == on) return;
  full_res = on;
  if (Q.rung >= LADDER_N) Q.rung = LADDER_N - 1;
  if (Q.mode == MODE_AUTO) set_rung(Q.rung);
  else if (Q.mode == MODE_PRESET) apply_preset((cw_quality)Q.level, -1);
  changed();
}
bool cw_quality_is_full_resolution(void) { return full_res; }

void cw_quality_auto(bool on, float budget_ms) {
  init_once();
  if (budget_ms > 0) Q.budget = budget_ms;
  if (locked() || Q.chosen) return;
  if (on && Q.mode != MODE_AUTO) auto_start(0);
  else if (!on && Q.mode == MODE_AUTO) {
    Q.mode = MODE_PRESET;
    cw_quality_set(CW_QUALITY_HIGH, false);
  }
}
bool cw_quality_is_auto(void) { return Q.mode == MODE_AUTO && !locked(); }
bool cw_quality_is_custom(void) { return Q.mode == MODE_CUSTOM; }
float cw_quality_auto_cost(void) { return Q.frames > 30 ? Q.avg : 0; }

/* the frame's GPU time (ms) where the host times the GPU: the current
 * tree's pipeline, every pass summed; <= 0 when it cannot */
static float gpu_frame_ms(void) {
  cw_tree *t = cw_current_tree();
  cw_pipeline *p = t ? cw_tree_slot(t, "pipeline") : NULL;
  return p ? cw_pipeline_gpu_ms(p) : 0;
}

void cw_quality_auto_measure(bool in_play) {
  /* the real clock, not cw_time(): a host may deliver a fixed step whatever the frame took (a libretro core does, for its
   * audio), and on that clock every frame looks on time */
  double now = (double)cw_profile_now_ns() / 1e9;
  double d = (now - Q.last_t) * 1000.0;
  bool was = Q.in_play;
  Q.last_t = now;
  Q.in_play = in_play;
  if (!in_play || !was || !cw_quality_is_auto()) return;
  if (d <= 0 || d > 250) return; /* a hitch (loading, a tab in the background) says nothing */
  /* the GPU's own time where timing it is cheap (native WebGPU's timestamps;
   * GL's timer queries cost the CPU a driver sync a frame): asked for while
   * Auto runs */
  float g = 0;
  if (cw_gpu_backend() == CW_GPU_WEBGPU) {
    cw__gpu_timing_request();
    g = gpu_frame_ms();
  }
  Q.gpu_timed = g > 0;
  float cost = Q.gpu_timed ? g : (float)d;
  float budget = Q.gpu_timed ? Q.budget * 0.75f : Q.budget;
  /* the first frames after a change settle (shaders, uploads, timers) before they count */
  if (++Q.frames <= 30) {
    Q.avg = cost;
    return;
  }
  Q.avg += (cost - Q.avg) * 0.05f;
  if (Q.frames > 150 && Q.avg <= budget) Q.retrying = false;
  if (Q.frames > 150 && Q.avg > budget && Q.rung < LADDER_N - 1) {
    cw_info("quality: auto %s %d%% -> %s %d%% (%s averaged %.1f ms, budget %.1f)", NAMES[RUNG(Q.rung).preset],
            (int)(RES[RUNG(Q.rung).res] * 100 + 0.5f), NAMES[RUNG(Q.rung + 1).preset],
            (int)(RES[RUNG(Q.rung + 1).res] * 100 + 0.5f), Q.gpu_timed ? "GPU time" : "frames", (double)Q.avg,
            (double)budget);
    Q.fail_rung = CW_MAX(Q.fail_rung, Q.rung);
    Q.retrying = false;
    set_rung(Q.rung + 1);
    changed();
  } else if (Q.gpu_timed && Q.frames > 300 && Q.avg < budget * 0.55f && Q.rung > 0 && Q.rung - 1 > Q.fail_rung) {
    cw_info("quality: auto %s %d%% -> %s %d%% (GPU time averaged %.1f ms, budget %.1f)", NAMES[RUNG(Q.rung).preset],
            (int)(RES[RUNG(Q.rung).res] * 100 + 0.5f), NAMES[RUNG(Q.rung - 1).preset],
            (int)(RES[RUNG(Q.rung - 1).res] * 100 + 0.5f), (double)Q.avg, (double)budget);
    set_rung(Q.rung - 1);
    changed();
  }
}

/* ── a settings entry ──────────────────────────────────────────────── */

/* the choices in order: the presets, then Auto */
static int choice_now(void) { return Q.mode == MODE_AUTO ? CW_QUALITY_COUNT : Q.level; }
static void choose(int c) {
  if (c == CW_QUALITY_COUNT) {
    Q.chosen = true;
    auto_start(Q.mode == MODE_AUTO ? Q.rung : 0);
  } else cw_quality_set((cw_quality)c, true);
}
void cw_quality_cycle(void) {
  init_once();
  if (forced) return;
  choose((choice_now() + 1) % (CW_QUALITY_COUNT + 1)); /* from Custom: the preset after the one it came from */
}
void cw_quality_cycle_back(void) {
  init_once();
  if (forced) return;
  int c = choice_now();
  if (Q.mode == MODE_CUSTOM) c++; /* back to the preset it came from */
  choose((c + CW_QUALITY_COUNT) % (CW_QUALITY_COUNT + 1));
}

const char *cw_quality_label(void) {
  return Q.mode == MODE_AUTO ? "Auto" : Q.mode == MODE_CUSTOM ? "Custom" : NAMES[Q.level];
}
const char *cw_render_res_label(void) {
  static char buf[16];
  int w = 0, h = 0;
  cw_render_size(&w, &h);
  snprintf(buf, sizeof buf, "%dp", h);
  return buf;
}

/* bits 0-2: 0 none, 1-4 a preset + 1, 5 Auto (bits 3-5 its level), 6 Custom
 * (bits 3-5 resolution, 6 ssao, 7-8 shadows, 9-10 aa, 11-12 effects, 13-20
 * the game's levers, 2 bits each): two bytes hold every choice of a game
 * with no levers of its own, three any game's */
uint32_t cw_quality_saved(void) {
  if (Q.mode == MODE_AUTO) { /* a level still being retried is not saved; a game's own Auto default counts once it has stepped */
    int rung = Q.rung + Q.retrying;
    return Q.chosen || rung > 0 ? 5u | (uint32_t)rung << 3 : 0;
  }
  if (!Q.chosen) return 0;
  if (Q.mode == MODE_PRESET) return (uint32_t)Q.level + 1;
  uint32_t w = 6u | (uint32_t)Q.v[CW_GFX_RESOLUTION] << 3 | (uint32_t)Q.v[CW_GFX_SSAO] << 6 | (uint32_t)Q.v[CW_GFX_SHADOWS] << 7 |
               (uint32_t)Q.v[CW_GFX_AA] << 9 | (uint32_t)Q.v[CW_GFX_EFFECTS] << 11;
  for (int i = 0; i < Q.n_levers; i++) w |= (uint32_t)(Q.v[CW_GFX_ENGINE_COUNT + i] & 3) << (13 + 2 * i);
  return w;
}
void cw_quality_restore(uint32_t saved) {
  init_once();
  if (locked()) return;
  uint32_t m = saved & 7;
  if (m >= 1 && m <= CW_QUALITY_COUNT) cw_quality_set((cw_quality)(m - 1), true);
  else if (m == 5) {
    Q.chosen = true;
    /* one level up from the save: a frame-timed run has no other way back, so one bad session would otherwise last for good */
    int rung = CW_CLAMP((int)(saved >> 3 & 7), 0, LADDER_N - 1);
    auto_start(rung > 0 ? rung - 1 : 0);
    Q.retrying = rung > 0;
  } else if (m == 6) {
    uint8_t v[CW_GFX_ENGINE_COUNT] = { (uint8_t)(saved >> 3 & 7), (uint8_t)(saved >> 6 & 1), (uint8_t)(saved >> 7 & 3),
                                       (uint8_t)(saved >> 9 & 3), (uint8_t)(saved >> 11 & 3) };
    for (int i = 0; i < CW_GFX_ENGINE_COUNT; i++) Q.v[i] = (uint8_t)CW_MIN(v[i], VALUES[i] - 1);
    for (int i = 0; i < Q.n_levers; i++) Q.v[CW_GFX_ENGINE_COUNT + i] = (uint8_t)CW_MIN((saved >> (13 + 2 * i)) & 3, (uint32_t)Q.levers[i].count - 1);
    Q.chosen = true;
    int p = matching_preset();
    Q.mode = p >= 0 ? MODE_PRESET : MODE_CUSTOM;
    if (p >= 0) Q.level = p;
    else Q.level = Q.v[CW_GFX_EFFECTS];
    changed();
  }
}

/* names alike ignoring case, spaces and underscores ("distant_ground" is "Distant ground") */
static bool same_name(const char *a, const char *b) {
  for (;;) {
    while (*a == ' ' || *a == '_') a++;
    while (*b == ' ' || *b == '_') b++;
    char x = (char)(*a >= 'A' && *a <= 'Z' ? *a + 32 : *a), y = (char)(*b >= 'A' && *b <= 'Z' ? *b + 32 : *b);
    if (x != y) return false;
    if (!x) return true;
    a++, b++;
  }
}
static int setting_by_name(const char *n) {
  static const char *const short_names[CW_GFX_ENGINE_COUNT] = { "resolution", "ssao", "shadows", "aa", "effects" };
  for (int i = 0; i < CW_GFX_ENGINE_COUNT; i++)
    if (same_name(n, short_names[i]) || same_name(n, SETTING_NAMES[i])) return i;
  for (int i = 0; i < Q.n_levers; i++)
    if (same_name(n, Q.levers[i].name)) return CW_GFX_ENGINE_COUNT + i;
  return -1;
}

static void cmd_quality(int argc, const char **argv, cw_reply *r) {
  /* an explicit request applies in a test run too (the same command on every
   * host: the run stays deterministic); only Auto and the player's saved
   * choice are locked there */
  if (argc > 1) {
    if (!strcasecmp(argv[1], "auto")) {
      if (!locked()) {
        Q.chosen = true;
        auto_start(0);
      }
    } else if (!strcasecmp(argv[1], "lock")) {
      cw__quality_lock();
    } else if (!strcasecmp(argv[1], "scope")) { /* scope push <setting>=<cap> ... | scope pop */
      if (argc > 2 && !strcasecmp(argv[2], "pop")) cw_quality_scope_pop();
      else if (argc > 3 && !strcasecmp(argv[2], "push")) {
        int8_t caps[CW_GFX_ENGINE_COUNT];
        memset(caps, -1, sizeof caps);
        for (int i = 3; i < argc; i++) {
          char nm[24];
          const char *eq = strchr(argv[i], '=');
          int s = -1;
          if (eq && eq - argv[i] < (int)sizeof nm) {
            memcpy(nm, argv[i], (size_t)(eq - argv[i]));
            nm[eq - argv[i]] = 0;
            s = setting_by_name(nm);
          }
          if (s < 0 || s >= CW_GFX_ENGINE_COUNT) {
            cw_reply_error(r, "quality scope push <resolution|ssao|shadows|aa|effects>=<highest value> ...");
            return;
          }
          caps[s] = (int8_t)atoi(eq + 1);
        }
        cw_quality_scope_push("debug", caps);
      } else {
        cw_reply_error(r, "quality scope push <setting>=<cap> ... | quality scope pop");
        return;
      }
    } else if (!strcasecmp(argv[1], "set")) {
      int s = argc > 3 ? setting_by_name(argv[2]) : -1;
      if (s < 0) {
        cw_reply_error(r, "quality set <resolution|ssao|shadows|aa|effects|a game setting> <value>");
        return;
      }
      Q.v[s] = (uint8_t)CW_CLAMP(atoi(argv[3]), 0, cw_gfx_value_count(s) - 1);
      int p = matching_preset();
      Q.mode = p >= 0 ? MODE_PRESET : MODE_CUSTOM;
      if (p >= 0) Q.level = p;
      changed();
    } else {
      int q = -1;
      for (int i = 0; i < CW_QUALITY_COUNT; i++)
        if (!strcasecmp(argv[1], NAMES[i])) q = i;
      if (q < 0) {
        cw_reply_error(r, "quality: low, medium, high, ultra, auto or set <setting> <value>");
        return;
      }
      Q.chosen = true;
      Q.mode = MODE_PRESET;
      apply_preset((cw_quality)q, -1);
      changed();
    }
  }
  cw_reply_printf(r, "{\"quality\":\"%s\",\"preset\":\"%s\",\"auto\":%s,\"locked\":%s,\"cost_ms\":%.2f,\"gpu_timed\":%s,\"settings\":{",
                  cw_quality_label(), NAMES[Q.level], cw_quality_is_auto() ? "true" : "false", locked() ? "true" : "false",
                  (double)cw_quality_auto_cost(), Q.gpu_timed ? "true" : "false");
  for (int i = 0; i < cw_gfx_count(); i++) cw_reply_printf(r, "%s\"%s\":\"%s\"", i ? "," : "", cw_gfx_name(i), cw_gfx_value_label(i));
  cw_reply_printf(r, "},\"particles\":%g,\"density\":%g}", (double)fx()->particles, (double)fx()->density);
}

/* ── scopes ───────────────────────────────────────────────────────── */

bool cw_quality_scope_push(const char *name, const int8_t caps[CW_GFX_ENGINE_COUNT]) {
  if (n_scopes >= SCOPE_MAX || !caps) {
    cw_warn("quality scope \"%s\": %s", name ? name : "", caps ? "too many open scopes" : "no caps");
    return false;
  }
  snprintf(SCOPES[n_scopes].name, sizeof SCOPES[n_scopes].name, "%s", name ? name : "");
  for (int i = 0; i < CW_GFX_ENGINE_COUNT; i++) SCOPES[n_scopes].cap[i] = caps[i] < 0 ? (int8_t)-1 : caps[i];
  n_scopes++;
  changed();
  return true;
}
void cw_quality_scope_pop(void) {
  if (n_scopes <= 0) return;
  n_scopes--;
  changed();
}
int cw_quality_scope_count(void) { return n_scopes; }

/* ── what the renderer asks ───────────────────────────────────────── */

void cw__quality_post(cw_post_settings *s) {
  const effects *e = fx();
  if (s->ssao) {
    if (!ev(CW_GFX_SSAO)) s->ssao = false;
    else if (e->ssao <= 1) {
      s->ssao_slices = 1;
      s->ssao_steps = s->ssao_steps > 4 ? 4 : s->ssao_steps;
    } else if (e->ssao == 3) {
      s->ssao_slices = s->ssao_slices < 3 ? 3 : s->ssao_slices;
      s->ssao_steps = s->ssao_steps < 8 ? 8 : s->ssao_steps;
    }
  }
  /* SSR: off on Low, fewer march steps on Medium, more on Ultra */
  if (s->ssr) {
    int fxl = ev(CW_GFX_EFFECTS);
    if (fxl == 0) s->ssr = false;
    else if (fxl == 1) s->ssr_quality *= 0.5f;
    else if (fxl == 3) s->ssr_quality = CW_MIN(1.0f, s->ssr_quality * 1.5f);
  }
  /* depth of field: off on Low */
  if (s->dof && ev(CW_GFX_EFFECTS) == 0) s->dof = false;
  /* SSGI: Ultra only (several ms of an integrated GPU even at half resolution) */
  if (s->ssgi && ev(CW_GFX_EFFECTS) < 3) s->ssgi = false;
  if (s->bloom && e->bloom_levels > 0 && s->bloom_levels > e->bloom_levels) s->bloom_levels = e->bloom_levels;
  int aa = ev(CW_GFX_AA);
  /* Off is no antialiasing at all: the post AA and the scene's MSAA (its cost back, on GL and WebGPU alike) */
  if (aa == AA_OFF) s->aa = CW_AA_NONE, s->msaa = 0;
  else if (aa == AA_FXAA && s->aa == CW_AA_SMAA) s->aa = CW_AA_FXAA;
}

int cw__quality_shadow_size(int authored) {
  int sh = ev(CW_GFX_SHADOWS);
  int s = authored;
  if (sh == SH_ULTRA) s = CW_MIN(authored * 2, 4096);
  else if (sh == SH_LOW || sh == SH_MEDIUM) s = CW_MIN(authored, 1024);
  return s < 256 ? (authored < 256 ? authored : 256) : s;
}
bool cw__quality_soft_shadows(void) { return ev(CW_GFX_SHADOWS) != SH_LOW; }
int cw__quality_shadow_level(void) { int sh = ev(CW_GFX_SHADOWS); return sh == SH_LOW ? 0 : sh == SH_MEDIUM ? 1 : sh == SH_HIGH ? 2 : 3; }
float cw__quality_lod_bias(void) { return fx()->lod_bias; }

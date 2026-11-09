/* Graphics settings (cartwheel/quality.h): the settings, the presets that
 * set them, Auto, and what the renderer asks of them (quality_internal.h). */
#include <math.h>
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
enum { RES_N = 6, RES_100 = CW_GFX_RES_100 };
enum { SH_MIN = CW_GFX_SHADOWS_MINIMAL, SH_LOW, SH_MEDIUM, SH_HIGH, SH_ULTRA, SH_N };
enum { AA_OFF = CW_GFX_AA_OFF, AA_FXAA, AA_SMAA, AA_FULL, AA_N };
static const int VALUES[CW_GFX_ENGINE_COUNT] = { RES_N, 2, SH_N, AA_N, CW_QUALITY_COUNT };
static const char *const SETTING_NAMES[CW_GFX_ENGINE_COUNT] = { "Resolution", "Ambient occlusion", "Shadows", "Anti-aliasing",
                                                                "Effects detail" };
static const char *const NAMES[CW_QUALITY_COUNT] = { "Very Low", "Low", "Medium", "High", "Ultra" };
static const char *const ONOFF[2] = { "Off", "On" };
static const char *const SHADOWS[SH_N] = { "Minimal", "Low", "Medium", "High", "Ultra" };
static const char *const AAS[AA_N] = { "Off", "FXAA", "SMAA", "Full" };

/* each preset's settings: ssao, shadows, aa, effects (Resolution is the player's, never a preset's) */
enum { P_SSAO, P_SHADOWS, P_AA, P_EFFECTS, P_N };
static const uint8_t PRESET[CW_QUALITY_COUNT][P_N] = {
  [CW_QUALITY_VERY_LOW] = { 0, SH_MIN, AA_FXAA, CW_QUALITY_VERY_LOW },
  [CW_QUALITY_LOW] = { 0, SH_LOW, AA_FXAA, CW_QUALITY_LOW },
  [CW_QUALITY_MEDIUM] = { 1, SH_MEDIUM, AA_SMAA, CW_QUALITY_MEDIUM },
  [CW_QUALITY_HIGH] = { 1, SH_HIGH, AA_FULL, CW_QUALITY_HIGH },
  [CW_QUALITY_ULTRA] = { 1, SH_ULTRA, AA_FULL, CW_QUALITY_ULTRA },
};
static const int P_SETTING[P_N] = { CW_GFX_SSAO, CW_GFX_SHADOWS, CW_GFX_AA, CW_GFX_EFFECTS };

/* effects detail */
typedef struct {
  float particles, density, lod_bias;
  float hrtf;       /* share of the HRTF voice limit (cartwheel/audio.h) */
  int bloom_levels; /* cap (0 = as authored, -1 = no bloom) */
  int ssao;         /* 1 one slice of 4 steps, 2 as authored, 3 raised */
  float lights;     /* clustered lights a cluster keeps (cartwheel/lighting.h max_per_cluster) */
  float distortion; /* share of the frame size the distortion buffer is drawn at (0 = no distortion) */
  float flash_lights; /* share of the explosion flash light pool used */
  float skin;         /* skin shading (cartwheel/skin_shading.h): 0 = plain PBR */
  float ssr;          /* SSR march quality scale (0 = off) */
  bool dof, ssgi;
  float shading; /* the lit materials' model: 1 = r187's physical one, 0 = the plain part (src/render/extensions/plain.mjs) */
} effects;
static const effects FX[CW_QUALITY_COUNT] = {
  [CW_QUALITY_VERY_LOW] = { 0.25f, 0.2f, 3.0f, 0.25f, -1, 1, 0.25f, 0, 0, 0, 0, false, false, 0 },
  [CW_QUALITY_LOW] = { 0.4f, 0.35f, 2.0f, 0.25f, 3, 1, 0.375f, 0, 0.25f, 0, 0, false, false, 1 },
  [CW_QUALITY_MEDIUM] = { 0.7f, 0.65f, 1.5f, 0.5f, 5, 1, 0.5f, 0.5f, 0.5f, 1, 0.5f, true, false, 1 },
  [CW_QUALITY_HIGH] = { 1, 1, 1, 1, 0, 2, 1, 1, 1, 1, 1, true, false, 1 },
  [CW_QUALITY_ULTRA] = { 1, 1, 0.75f, 1, 0, 3, 1, 1, 1, 1, 1.5f, true, true, 1 },
};

/* the shadows setting: caster culling (bounding sphere radius / distance under this skips the caster), cascades */
static const float CASTER_MIN[SH_N] = { 0.02f, 0.012f, 0.006f, 0, 0 };
static const int CASCADES[SH_N] = { 1, 2, 3, 4, 4 };

/* Auto's ladder, top first: each rung a full setting vector (one change from the rung above), its levers its
 * preset's; res -1 keeps the player's Resolution (cw_quality_full_resolution(false) adds the rungs below Very Low) */
typedef struct {
  uint8_t level, v[P_N];
  int8_t res;
  float ratio; /* the expected cost against High (refined in the run by what each rung measured) */
} rung_t;
static const rung_t LADDER[] = {
  { CW_QUALITY_HIGH, { 1, SH_HIGH, AA_FULL, CW_QUALITY_HIGH }, -1, 1.0f },
  { CW_QUALITY_HIGH, { 1, SH_MEDIUM, AA_FULL, CW_QUALITY_HIGH }, -1, 0.85f },
  { CW_QUALITY_MEDIUM, { 1, SH_MEDIUM, AA_SMAA, CW_QUALITY_MEDIUM }, -1, 0.6f },
  { CW_QUALITY_MEDIUM, { 0, SH_MEDIUM, AA_SMAA, CW_QUALITY_MEDIUM }, -1, 0.52f },
  { CW_QUALITY_LOW, { 0, SH_LOW, AA_FXAA, CW_QUALITY_LOW }, -1, 0.4f },
  { CW_QUALITY_LOW, { 0, SH_LOW, AA_FXAA, CW_QUALITY_VERY_LOW }, -1, 0.33f },
  { CW_QUALITY_VERY_LOW, { 0, SH_MIN, AA_FXAA, CW_QUALITY_VERY_LOW }, -1, 0.25f },
  /* cw_quality_full_resolution(false) only */
  { CW_QUALITY_VERY_LOW, { 0, SH_MIN, AA_FXAA, CW_QUALITY_VERY_LOW }, CW_GFX_RES_85, 0.25f * 0.72f },
  { CW_QUALITY_VERY_LOW, { 0, SH_MIN, AA_FXAA, CW_QUALITY_VERY_LOW }, CW_GFX_RES_75, 0.25f * 0.56f },
  { CW_QUALITY_VERY_LOW, { 0, SH_MIN, AA_FXAA, CW_QUALITY_VERY_LOW }, CW_GFX_RES_67, 0.25f * 0.45f },
  { CW_QUALITY_VERY_LOW, { 0, SH_MIN, AA_FXAA, CW_QUALITY_VERY_LOW }, CW_GFX_RES_60, 0.25f * 0.36f },
  { CW_QUALITY_VERY_LOW, { 0, SH_MIN, AA_FXAA, CW_QUALITY_VERY_LOW }, CW_GFX_RES_50, 0.25f * 0.25f },
};
enum { LADDER_FULL_N = 7, LADDER_ALL_N = (int)(sizeof LADDER / sizeof LADDER[0]) };
static bool full_res = true;
#define LADDER_N (full_res ? LADDER_FULL_N : LADDER_ALL_N)
/* the old engine's Auto words (code 5): their rung's preset on this ladder, and (the resolution ladder) its resolution */
static const uint8_t OLD_FULL_RUNG[3] = { 0, 2, 4 };
static const uint8_t OLD_RUNG[7] = { 0, 0, 2, 2, 4, 4, 4 };
static const uint8_t OLD_RUNG_RES[7] = { CW_GFX_RES_100, CW_GFX_RES_85, CW_GFX_RES_75, CW_GFX_RES_67, CW_GFX_RES_67, CW_GFX_RES_60, CW_GFX_RES_50 };
/* and the resolution an old preset word (1..4: Low..Ultra) drew at without full resolution */
static const uint8_t OLD_PRESET_RES[4] = { CW_GFX_RES_67, CW_GFX_RES_75, CW_GFX_RES_100, CW_GFX_RES_100 };

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
  uint8_t user_res; /* the player's Resolution (a resolution rung of Auto draws below it) */
  lever levers[CW_GFX_MAX_LEVERS];
  int n_levers;
  bool chosen;     /* the player (or a save) chose: the game's Auto default no longer applies */
  bool keep_msaa;  /* cw_quality_keep_msaa */
  /* Auto */
  int32_t rung;
  int fail_rung;   /* the highest rung over budget in this run (-1 none) */
  float budget;
  bool in_play, gpu_timed;
  bool retrying;   /* a run trying the rung above its saved one: unproven until it has held for a while */
  double last_t;
  float avg;
  int frames;
  float rung_cost[LADDER_ALL_N]; /* what each rung averaged when it last held a while (0 = never) */
  cw_quality_change_fn fn;
  void *fn_user;
} Q = { .level = CW_QUALITY_HIGH, .budget = 19, .fail_rung = -1, .user_res = RES_100,
        .v = { RES_100, 1, SH_HIGH, AA_FULL, CW_QUALITY_HIGH } };

/* scopes (cw_quality_scope_push): caps on top of the player's settings, innermost last */
enum { SCOPE_MAX = 4 };
static struct { char name[24]; int8_t cap[CW_GFX_ENGINE_COUNT]; } SCOPES[SCOPE_MAX];
static int n_scopes;
static bool flat; /* CW_TEST_QUALITY_FLAT (native): every preset draws High's settings (the range gate's control) */
/* gate controls (native, tests/quality-unit.mjs): the old engine's behaviour back, one at a time */
static bool old_res;  /* CW_TEST_QUALITY_OLD_RES: a preset sets Resolution to 100% and only 100% matches a preset */
static bool v2_only;  /* CW_TEST_QUALITY_V2_ONLY: every saved word read with the new layout */
static bool no_jump;  /* CW_TEST_QUALITY_NO_JUMP: Auto steps one rung at a time, never jumps */
/* a setting as drawn: the player's value, lowered by every scope that caps it */
static int ev(int s) {
  int v = Q.v[s];
  if (flat && s != CW_GFX_RESOLUTION) v = s == CW_GFX_SSAO ? 1 : s == CW_GFX_SHADOWS ? SH_HIGH : s == CW_GFX_AA ? AA_FULL : CW_QUALITY_HIGH;
  for (int i = 0; i < n_scopes; i++)
    if (SCOPES[i].cap[s] >= 0 && v > SCOPES[i].cap[s]) v = SCOPES[i].cap[s];
  return v;
}

static void cmd_quality(int argc, const char **argv, cw_reply *r);
static bool forced; /* CW_QUALITY (native): this preset whatever else asks */

static bool env_on(const char *k) {
#if !CW_CART
  const char *e = getenv(k);
  return e && atoi(e) > 0;
#else
  (void)k;
  return false;
#endif
}

static void init_once(void) {
  if (Q.inited) return;
  Q.inited = true;
  old_res = env_on("CW_TEST_QUALITY_OLD_RES");
  v2_only = env_on("CW_TEST_QUALITY_V2_ONLY");
  no_jump = env_on("CW_TEST_QUALITY_NO_JUMP");
  if (old_res || v2_only || no_jump) cw_warn("quality: a gate control is on (CW_TEST_QUALITY_OLD_RES / V2_ONLY / NO_JUMP)");
  cw_debug_field("cw.quality", &Q.level, CW_DBG_I32, 1);
  cw_debug_field("cw.quality_mode", &Q.mode, CW_DBG_I32, 1);
  cw_debug_field("cw.quality_rung", &Q.rung, CW_DBG_I32, 1);
  cw_debug_field("cw.quality_scopes", &n_scopes, CW_DBG_I32, 1);
  cw_debug_command("quality",
                   "quality [verylow|low|medium|high|ultra|auto|lock | set <setting> <value> | scope push <setting>=<cap>... | scope pop]: "
                   "the graphics settings (cartwheel/quality.h); settings and values (a name or its number): resolution 50|60|67|75|85|100 "
                   "(0-5), ssao off|on (0-1), shadows minimal|low|medium|high|ultra (0-4), aa off|fxaa|smaa|full (0-3), effects "
                   "verylow|low|medium|high|ultra (0-4), or a game setting's name (spaces as _) and its value's label or number",
                   cmd_quality);
  cw__quality_menu_register();
}

/* Auto, and a saved choice, never apply in a deterministic run (every host
 * starts at High); a choice made in it does (it comes from input every host
 * replays alike). CW_QUALITY fixes the preset against everything. */
static bool test_lock; /* cw__quality_lock: a test host that is not a deterministic run */
static bool locked(void) { return forced || test_lock || cw_is_deterministic_run(); }

static void auto_start(int rung);
static void apply_preset(cw_quality q);
static void changed(void);

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

/* every setting of preset q but Resolution, which stays the player's */
static void apply_preset(cw_quality q) {
  for (int i = 0; i < P_N; i++) Q.v[P_SETTING[i]] = PRESET[q][i];
  Q.v[CW_GFX_RESOLUTION] = old_res ? RES_100 : Q.user_res;
  if (old_res) Q.user_res = RES_100;
  for (int i = 0; i < Q.n_levers; i++) Q.v[CW_GFX_ENGINE_COUNT + i] = Q.levers[i].preset[q];
  Q.level = q;
}

/* the preset every setting but Resolution matches, else -1 */
static int matching_preset(void) {
  for (int q = 0; q < CW_QUALITY_COUNT; q++) {
    bool same = true;
    if (old_res && Q.v[CW_GFX_RESOLUTION] != RES_100) same = false;
    for (int i = 0; i < P_N && same; i++) same = Q.v[P_SETTING[i]] == PRESET[q][i];
    for (int i = 0; i < Q.n_levers && same; i++) same = Q.v[CW_GFX_ENGINE_COUNT + i] == Q.levers[i].preset[q];
    if (same) return q;
  }
  return -1;
}

static void set_rung(int rung) {
  Q.rung = CW_CLAMP(rung, 0, LADDER_N - 1);
  const rung_t *r = &LADDER[Q.rung];
  for (int i = 0; i < P_N; i++) Q.v[P_SETTING[i]] = r->v[i];
  Q.v[CW_GFX_RESOLUTION] = r->res >= 0 ? (uint8_t)r->res : Q.user_res;
  for (int i = 0; i < Q.n_levers; i++) Q.v[CW_GFX_ENGINE_COUNT + i] = Q.levers[i].preset[r->level];
  Q.level = r->level;
}

/* a preset's name as typed: "Very Low", "verylow", "very_low", "vlow" */
static bool same_name(const char *a, const char *b);
static int preset_by_name(const char *s) {
  for (int i = 0; i < CW_QUALITY_COUNT; i++)
    if (same_name(s, NAMES[i])) return i;
  if (same_name(s, "vlow")) return CW_QUALITY_VERY_LOW;
  return -1;
}

void cw__quality_init(void) {
  init_once();
  if (cw_is_deterministic_run()) { /* every host draws the same frame */
    apply_preset(CW_QUALITY_HIGH);
    Q.mode = MODE_PRESET;
  }
#if !CW_CART
  /* CW_QUALITY=verylow|low|medium|high|ultra: a fixed preset (comparing presets in a test run) */
  const char *env = getenv("CW_QUALITY");
  int q = env && *env ? preset_by_name(env) : -1;
  if (q >= 0) {
    apply_preset((cw_quality)q);
    Q.mode = MODE_PRESET;
    forced = true;
  }
  const char *fl = getenv("CW_TEST_QUALITY_FLAT");
  flat = fl && atoi(fl) > 0;
  if (flat) cw_warn("CW_TEST_QUALITY_FLAT: every preset draws High's settings (a gate control)");
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
  if (Q.mode == MODE_AUTO) { /* the game or a tool: Auto goes on from the first rung at or below that preset */
    int r = 0;
    while (r < LADDER_N - 1 && LADDER[r].level > q) r++;
    set_rung(r);
  } else {
    Q.mode = MODE_PRESET;
    apply_preset(q);
  }
  changed();
}

void cw_quality_keep_msaa(bool on) { Q.keep_msaa = on; }

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

static const char *value_name(int s, int v) {
  static char buf[48];
  switch (s) {
  case CW_GFX_RESOLUTION: snprintf(buf, sizeof buf, "%d%%", (int)(RES[v] * 100 + 0.5f)); return buf;
  case CW_GFX_SSAO: return ONOFF[v];
  case CW_GFX_SHADOWS: return SHADOWS[v];
  case CW_GFX_AA: return AAS[v];
  case CW_GFX_EFFECTS: return NAMES[v];
  default: return Q.levers[s - CW_GFX_ENGINE_COUNT].values[v];
  }
}

const char *cw_gfx_value_label(int s) {
  static char buf[48];
  if (!valid(s)) return "?";
  int v = Q.v[s];
  if (s == CW_GFX_RESOLUTION) {
    int w = 0, h = 0;
    cw__output_size(&w, &h);
    float k = RES[v];
    if (w > 0 && h > 0)
      snprintf(buf, sizeof buf, "%d%%  %dx%d", (int)(k * 100 + 0.5f), CW_MAX(1, (int)(w * k + 0.5f)), CW_MAX(1, (int)(h * k + 0.5f)));
    else snprintf(buf, sizeof buf, "%d%%", (int)(k * 100 + 0.5f));
    return buf;
  }
  return value_name(s, v);
}

/* a setting set by the player or a tool: Resolution alone touches nothing else */
static void set_value(int s, int value) {
  value = CW_CLAMP(value, 0, cw_gfx_value_count(s) - 1);
  Q.chosen = true;
  if (s == CW_GFX_RESOLUTION) {
    Q.user_res = (uint8_t)value;
    if (Q.mode != MODE_AUTO || LADDER[Q.rung].res < 0) Q.v[s] = (uint8_t)value;
    if (old_res) { /* the old engine: Resolution counted in the match (100% only), so another is Custom */
      int p = matching_preset();
      Q.mode = p >= 0 ? MODE_PRESET : MODE_CUSTOM;
    }
    changed();
    return;
  }
  Q.v[s] = (uint8_t)value;
  int p = matching_preset();
  if (p >= 0) {
    Q.mode = MODE_PRESET;
    Q.level = p;
  } else Q.mode = MODE_CUSTOM;
  changed();
}

void cw_gfx_set(int s, int value) {
  init_once();
  if (forced || !valid(s)) return;
  set_value(s, value);
}

void cw_gfx_step(int s, int dir) {
  int n = cw_gfx_value_count(s);
  if (n <= 0) return;
  cw_gfx_set(s, ((cw_gfx_get(s) + (dir < 0 ? -1 : 1)) % n + n) % n);
}

int cw_quality_lever_add(const char *name, const char *const *values, int value_count, cw_quality_lever_presets presets) {
  init_once();
  if (Q.n_levers >= CW_GFX_MAX_LEVERS || !name || !values || value_count < 1) return -1;
  lever *l = &Q.levers[Q.n_levers];
  l->name = name;
  l->values = values;
  l->count = CW_MIN(value_count, CW_GFX_LEVER_MAX_VALUES);
  const uint8_t p[CW_QUALITY_COUNT] = { presets.very_low, presets.low, presets.medium, presets.high, presets.ultra };
  for (int q = 0; q < CW_QUALITY_COUNT; q++) l->preset[q] = (uint8_t)CW_CLAMP(p[q], 0, l->count - 1);
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
  if (!strcmp(knob, "dispersion")) return 0; /* distortion is colourless: no preset splits its colours */
  if (!strcmp(knob, "flash_lights")) return fx()->flash_lights;
  if (!strcmp(knob, "skin")) return fx()->skin;
  if (!strcmp(knob, "shading")) return fx()->shading;
  return 1;
}
float cw_quality_density(void) { return fx()->density; }
bool cw__quality_small_hdr(void) { return ev(CW_GFX_EFFECTS) <= CW_QUALITY_LOW; }
bool cw__quality_light_bloom(void) { return ev(CW_GFX_EFFECTS) <= CW_QUALITY_LOW; }
bool cw__quality_fused_post(void) { return ev(CW_GFX_EFFECTS) <= CW_QUALITY_LOW; }
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
  memset(Q.rung_cost, 0, sizeof Q.rung_cost);
  set_rung(rung);
  changed();
}

void cw_quality_full_resolution(bool on) {
  init_once();
  if (full_res == on) return;
  full_res = on;
  if (Q.rung >= LADDER_N) Q.rung = LADDER_N - 1;
  if (Q.mode == MODE_AUTO) set_rung(Q.rung);
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

/* a rung's expected cost: what it measured in this run, else the cost now scaled by the ladder's ratios */
static float expected_cost(int r, float now) {
  if (Q.rung_cost[r] > 0) return Q.rung_cost[r];
  return now * LADDER[r].ratio / LADDER[Q.rung].ratio;
}

static void log_step(int to, const char *why, float budget) {
  const rung_t *a = &LADDER[Q.rung], *b = &LADDER[to];
  cw_info("quality: auto %s rung %d -> %s rung %d (%s %s averaged %.1f ms, budget %.1f)", NAMES[a->level], Q.rung, NAMES[b->level], to,
          why, Q.gpu_timed ? "GPU time" : "frames", (double)Q.avg, (double)budget);
}

#if !CW_CART
static double test_slow_ms = -1; /* CW_TEST_FRAME_MS (native): every frame of play takes this long on Auto's clock */
#endif

void cw_quality_auto_measure(bool in_play) {
  /* the real clock, not cw_time(): a host may deliver a fixed step whatever the frame took (a libretro core does, for its
   * audio), and on that clock every frame looks on time */
  double now = (double)cw_profile_now_ns() / 1e9;
  double d = (now - Q.last_t) * 1000.0;
  bool was = Q.in_play;
  Q.last_t = now;
  Q.in_play = in_play;
#if !CW_CART
  if (test_slow_ms < 0) {
    const char *e = getenv("CW_TEST_FRAME_MS");
    test_slow_ms = e && atof(e) > 0 ? atof(e) : 0;
  }
#endif
  if (!in_play || !was || !cw_quality_is_auto()) return;
#if !CW_CART
  /* the slow-frame host: each rung's frame time as the ladder's ratio of High's */
  if (test_slow_ms > 0) d = test_slow_ms * LADDER[Q.rung].ratio;
#endif
  if (d <= 0 || d > 250) return; /* a hitch (loading, a tab in the background) says nothing */
  /* the GPU's own time where timing it is cheap (native WebGPU's timestamps;
   * GL's timer queries cost the CPU a driver sync a frame): asked for while
   * Auto runs */
  float g = 0;
#if !CW_CART
  if (cw_gpu_backend() == CW_GPU_WEBGPU && test_slow_ms <= 0) {
#else
  if (cw_gpu_backend() == CW_GPU_WEBGPU) {
#endif
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
  if (Q.frames > 150) Q.rung_cost[Q.rung] = Q.avg;
  /* a big overshoot a second into a rung: straight to the rung expected to fit (every rung above it is over) */
  if (!no_jump && Q.frames == 90 && Q.avg > budget * 1.6f && Q.rung < LADDER_N - 1) {
    int to = Q.rung + 1;
    while (to < LADDER_N - 1 && expected_cost(to, Q.avg) > budget) to++;
    log_step(to, "jump:", budget);
    Q.rung_cost[Q.rung] = Q.avg;
    Q.fail_rung = CW_MAX(Q.fail_rung, to - 1);
    Q.retrying = false;
    set_rung(to);
    changed();
    return;
  }
  if (Q.frames > 150 && Q.avg <= budget) Q.retrying = false;
  if (Q.frames > 150 && Q.avg > budget && Q.rung < LADDER_N - 1) {
    log_step(Q.rung + 1, "step:", budget);
    Q.fail_rung = CW_MAX(Q.fail_rung, Q.rung);
    Q.retrying = false;
    set_rung(Q.rung + 1);
    changed();
  } else if (Q.gpu_timed && Q.frames > 300 && Q.avg < budget * 0.55f && Q.rung > 0 && Q.rung - 1 > Q.fail_rung) {
    log_step(Q.rung - 1, "up:", budget);
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

/* The save word.
 *   v2 (written now): bits 0-2 = 7; bits 3-5 the choice (0..4 a preset, 5 Auto, 6 Custom); bits 6-8 Resolution;
 *   Auto: bits 9-12 the rung; Custom: bit 13 ambient occlusion, 14-16 shadows, 17-18 anti-aliasing, 19-21 effects,
 *   22-29 the game's levers (2 bits each). 30 bits.
 *   Older words (read as before): 0 none, 1-4 Low..Ultra + 1, 5 Auto (its old ladder's rung in bits 3-5), 6 Custom
 *   (bits 3-5 resolution, 6 ssao, 7-8 shadows Low..Ultra, 9-10 aa Off/FXAA/SMAA, 11-12 effects Low..Ultra, 13-20 levers). */
enum { W_V2 = 7, C_AUTO = 5, C_CUSTOM = 6 };
uint32_t cw_quality_saved(void) {
  uint32_t res = (uint32_t)Q.user_res << 6;
  if (Q.mode == MODE_AUTO) { /* a rung still being retried is not saved; a game's own Auto default counts once it has stepped */
    int rung = CW_MIN(Q.rung + Q.retrying, LADDER_N - 1);
    return Q.chosen || rung > 0 ? W_V2 | (uint32_t)C_AUTO << 3 | res | (uint32_t)rung << 9 : 0;
  }
  if (!Q.chosen) return 0;
  if (Q.mode == MODE_PRESET) return W_V2 | (uint32_t)Q.level << 3 | res;
  uint32_t w = W_V2 | (uint32_t)C_CUSTOM << 3 | res | (uint32_t)Q.v[CW_GFX_SSAO] << 13 | (uint32_t)Q.v[CW_GFX_SHADOWS] << 14 |
               (uint32_t)Q.v[CW_GFX_AA] << 17 | (uint32_t)Q.v[CW_GFX_EFFECTS] << 19;
  for (int i = 0; i < Q.n_levers; i++) w |= (uint32_t)(Q.v[CW_GFX_ENGINE_COUNT + i] & 3) << (22 + 2 * i);
  return w;
}

/* a Custom choice's settings (Resolution already set): its preset if they match one */
static void restore_custom(const uint8_t v[P_N], uint32_t levers) {
  for (int i = 0; i < P_N; i++) Q.v[P_SETTING[i]] = (uint8_t)CW_MIN(v[i], VALUES[P_SETTING[i]] - 1);
  for (int i = 0; i < Q.n_levers; i++) Q.v[CW_GFX_ENGINE_COUNT + i] = (uint8_t)CW_MIN((levers >> (2 * i)) & 3, (uint32_t)Q.levers[i].count - 1);
  Q.chosen = true;
  int p = matching_preset();
  Q.mode = p >= 0 ? MODE_PRESET : MODE_CUSTOM;
  Q.level = p >= 0 ? p : Q.v[CW_GFX_EFFECTS];
  changed();
}
/* Auto from a saved rung: one rung up (a frame-timed run has no other way back, so one bad session would otherwise last for good) */
static void restore_auto(int rung) {
  Q.chosen = true;
  rung = CW_CLAMP(rung, 0, LADDER_N - 1);
  auto_start(rung > 0 ? rung - 1 : 0);
  Q.retrying = rung > 0;
}
static void restore_res(int res) {
  Q.user_res = (uint8_t)CW_CLAMP(res, 0, RES_N - 1);
  Q.v[CW_GFX_RESOLUTION] = Q.user_res;
}

void cw_quality_restore(uint32_t saved) {
  init_once();
  if (locked()) return;
  uint32_t m = saved & 7;
  if (m == W_V2 || (v2_only && saved)) {
    uint32_t c = saved >> 3 & 7;
    restore_res((int)(saved >> 6 & 7));
    if (c < CW_QUALITY_COUNT) cw_quality_set((cw_quality)c, true);
    else if (c == C_AUTO) restore_auto((int)(saved >> 9 & 15));
    else if (c == C_CUSTOM) {
      uint8_t v[P_N] = { (uint8_t)(saved >> 13 & 1), (uint8_t)(saved >> 14 & 7), (uint8_t)(saved >> 17 & 3), (uint8_t)(saved >> 19 & 7) };
      restore_custom(v, saved >> 22);
    }
    return;
  }
  /* an older engine's word */
  if (m >= 1 && m <= 4) { /* Low..Ultra: then at 100%, or the preset's own resolution without full resolution */
    restore_res(full_res ? RES_100 : OLD_PRESET_RES[m - 1]);
    cw_quality_set((cw_quality)m, true);
  } else if (m == 5) {
    int old = (int)(saved >> 3 & 7);
    if (full_res) {
      restore_res(RES_100);
      restore_auto(OLD_FULL_RUNG[CW_MIN(old, 2)]);
    } else {
      restore_res(OLD_RUNG_RES[CW_MIN(old, 6)]);
      restore_auto(OLD_RUNG[CW_MIN(old, 6)]);
    }
  } else if (m == 6) {
    restore_res((int)(saved >> 3 & 7));
    static const uint8_t OLD_AA[4] = { AA_OFF, AA_FXAA, AA_FULL, AA_FULL }; /* the old SMAA kept the scene's MSAA: Full */
    uint8_t v[P_N] = { (uint8_t)(saved >> 6 & 1), (uint8_t)((saved >> 7 & 3) + 1), OLD_AA[saved >> 9 & 3], (uint8_t)((saved >> 11 & 3) + 1) };
    restore_custom(v, saved >> 13);
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
/* a value as typed: its label ("minimal", "smaa", "75" or "75%" for Resolution), else its number; -1 unknown */
static int value_by_name(int s, const char *t) {
  for (int v = 0; v < cw_gfx_value_count(s); v++) {
    const char *nm = value_name(s, v);
    if (same_name(t, nm)) return v;
    if (s == CW_GFX_RESOLUTION) {
      char pct[8];
      snprintf(pct, sizeof pct, "%d", (int)(RES[v] * 100 + 0.5f));
      if (same_name(t, pct)) return v;
    }
  }
  if (s == CW_GFX_EFFECTS) {
    int q = preset_by_name(t);
    if (q >= 0) return q;
  }
  char *end;
  long v = strtol(t, &end, 10);
  return *t && !*end ? (int)v : -1;
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
          int v = s >= 0 && s < CW_GFX_ENGINE_COUNT ? value_by_name(s, eq + 1) : -1;
          if (v < 0) {
            cw_reply_error(r, "quality scope push <resolution|ssao|shadows|aa|effects>=<highest value> ...");
            return;
          }
          caps[s] = (int8_t)v;
        }
        cw_quality_scope_push("debug", caps);
      } else {
        cw_reply_error(r, "quality scope push <setting>=<cap> ... | quality scope pop");
        return;
      }
    } else if (!strcasecmp(argv[1], "set")) {
      int s = argc > 3 ? setting_by_name(argv[2]) : -1;
      int v = s >= 0 ? value_by_name(s, argv[3]) : -1;
      if (s < 0 || v < 0) {
        cw_reply_error(r, "quality set <resolution|ssao|shadows|aa|effects|a game setting> <value>");
        return;
      }
      bool was_chosen = Q.chosen;
      set_value(s, v);
      Q.chosen = was_chosen || Q.chosen;
    } else {
      int q = preset_by_name(argv[1]);
      if (q < 0) {
        cw_reply_error(r, "quality: verylow, low, medium, high, ultra, auto or set <setting> <value>");
        return;
      }
      Q.chosen = true;
      Q.mode = MODE_PRESET;
      apply_preset((cw_quality)q);
      changed();
    }
  }
  cw_reply_printf(r, "{\"quality\":\"%s\",\"preset\":\"%s\",\"auto\":%s,\"rung\":%d,\"locked\":%s,\"cost_ms\":%.2f,\"gpu_timed\":%s,\"saved\":%u,\"settings\":{",
                  cw_quality_label(), NAMES[Q.level], cw_quality_is_auto() ? "true" : "false", Q.rung, locked() ? "true" : "false",
                  (double)cw_quality_auto_cost(), Q.gpu_timed ? "true" : "false", cw_quality_saved());
  for (int i = 0; i < cw_gfx_count(); i++) cw_reply_printf(r, "%s\"%s\":\"%s\"", i ? "," : "", cw_gfx_name(i), cw_gfx_value_label(i));
  cw_reply_printf(r, "},\"particles\":%g,\"density\":%g,\"flat\":%s}", (double)fx()->particles, (double)fx()->density, flat ? "true" : "false");
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
  /* SSR: off below Medium, fewer march steps on Medium, more on Ultra */
  if (s->ssr) {
    if (e->ssr <= 0) s->ssr = false;
    else s->ssr_quality = CW_MIN(1.0f, s->ssr_quality * e->ssr);
  }
  if (s->dof && !e->dof) s->dof = false;
  /* SSGI: Ultra only (several ms of an integrated GPU even at half resolution) */
  if (s->ssgi && !e->ssgi) s->ssgi = false;
  if (s->bloom && e->bloom_levels < 0) s->bloom = false;
  else if (s->bloom && e->bloom_levels > 0 && s->bloom_levels > e->bloom_levels) s->bloom_levels = e->bloom_levels;
  int aa = ev(CW_GFX_AA);
  /* Off is no antialiasing at all: the post AA and the scene's MSAA (its cost back, on GL and WebGPU alike) */
  if (aa == AA_OFF) s->aa = CW_AA_NONE, s->msaa = 0;
  /* TAA is the scene's own antialiasing too: FXAA (Very Low, Low) draws without it and its velocity pass */
  bool had_taa = s->taa;
  if (aa <= AA_FXAA) s->taa = false;
  if (aa == AA_FXAA || aa == AA_SMAA) {
    /* the scene's MSAA off (unless the game keeps it), the post filter in its place; a scene with no AA at all keeps none */
    bool any = s->aa != CW_AA_NONE || s->msaa > 1 || had_taa;
    bool keep = Q.keep_msaa && s->msaa > 1;
    if (!keep) s->msaa = 0;
    if (aa == AA_FXAA) s->aa = any && !(keep && s->aa == CW_AA_NONE) ? CW_AA_FXAA : s->aa;
    else if (any && s->aa != CW_AA_FXAA && !(keep && s->aa == CW_AA_NONE)) s->aa = CW_AA_SMAA;
  }
}

static int shadows(void) { return ev(CW_GFX_SHADOWS); }
int cw__quality_shadow_size(int authored) {
  int sh = shadows();
  int s = authored;
  if (sh == SH_ULTRA) s = CW_MIN(authored * 2, 4096);
  else if (sh == SH_LOW || sh == SH_MEDIUM) s = CW_MIN(authored, 1024);
  else if (sh == SH_MIN) s = CW_CLAMP(authored / 2, 512, 1024);
  if (s > authored && sh != SH_ULTRA) s = authored; /* never above what the light asks below Ultra */
  return s < 256 ? (authored < 256 ? authored : 256) : s;
}
bool cw__quality_soft_shadows(void) { return shadows() >= SH_MEDIUM; }
int cw__quality_shadow_level(void) { return shadows(); }
int cw__quality_cascades(int authored) { return CW_MIN(authored, CASCADES[shadows()]); }
float cw__quality_shadow_distance(void) { int sh = shadows(); return sh == SH_MIN ? 0.5f : sh == SH_ULTRA ? 1.5f : 1.0f; }
bool cw__quality_local_shadows(void) { return shadows() > SH_MIN; }
float cw__quality_caster_min(void) { return CASTER_MIN[shadows()]; }
float cw__quality_lod_bias(void) { return fx()->lod_bias; }
int cw__quality_effects(void) { return ev(CW_GFX_EFFECTS); }

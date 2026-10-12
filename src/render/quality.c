/* Graphics quality presets (cartwheel/quality.h): the table, the automatic
 * step-down and what the renderer asks of them (quality_internal.h). */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "cartwheel/quality.h"
#include "cartwheel/debug.h"
#include "cartwheel/core.h"
#include "cartwheel/platform.h"
#include "quality_internal.h"

typedef struct {
  float particles, density;
  int ssao; /* 0 off, 1 reduced, 2 as authored, 3 raised */
  int bloom_levels; /* cap (0 = as authored) */
  int shadow_cap, shadow_mul;
  int aa; /* 0 off, 1 at most FXAA, 2 as authored, 3 SMAA when any */
  bool soft_shadows;
} preset;

static const preset P[CW_QUALITY_COUNT] = {
  [CW_QUALITY_LOW] = { 0.4f, 0.35f, 0, 3, 1024, 1, 0, false },
  [CW_QUALITY_MEDIUM] = { 0.7f, 0.65f, 1, 5, 2048, 1, 1, true },
  [CW_QUALITY_HIGH] = { 1, 1, 2, 0, 0, 1, 2, true },
  [CW_QUALITY_ULTRA] = { 1, 1, 3, 0, 4096, 2, 3, true },
};

static struct {
  bool inited;
  int32_t level;    /* cw.quality */
  bool user;        /* the player chose: no automatic steps */
  bool auto_on;
  float budget;
  bool in_play;
  double last_t;
  float avg;
  int frames;
  cw_quality_change_fn fn;
  void *fn_user;
} Q = { .level = CW_QUALITY_HIGH, .auto_on = false, .budget = 19 }; /* automatic choice: off unless the game turns it on */

static const char *NAMES[CW_QUALITY_COUNT] = { "Low", "Medium", "High", "Ultra" };

static void cmd_quality(int argc, const char **argv, cw_reply *r);
static bool forced; /* CW_QUALITY (native): this preset whatever else asks */

static void init_once(void) {
  if (Q.inited) return;
  Q.inited = true;
  cw_debug_field("cw.quality", &Q.level, CW_DBG_I32, 1);
  cw_debug_command("quality", "quality [low|medium|high|ultra|auto]: the graphics preset (cartwheel/quality.h)", cmd_quality);
}

void cw__quality_init(void) {
  init_once();
  if (cw_is_deterministic_run()) { /* every host draws the same frame */
    Q.level = CW_QUALITY_HIGH;
    Q.auto_on = false;
  }
#if !CW_CART
  /* CW_QUALITY=low|medium|high|ultra: a fixed preset (comparing presets in a test run) */
  const char *env = getenv("CW_QUALITY");
  for (int i = 0; env && *env && i < CW_QUALITY_COUNT; i++)
    if (!strcasecmp(env, NAMES[i])) {
      Q.level = i;
      Q.auto_on = false;
      forced = true;
    }
#endif
}

static bool locked(void) { return forced || cw_is_deterministic_run(); }

cw_quality cw_quality_get(void) { return (cw_quality)Q.level; }
const char *cw_quality_name(cw_quality q) { return q >= 0 && q < CW_QUALITY_COUNT ? NAMES[q] : "?"; }

static void change(cw_quality q) {
  if (q == (cw_quality)Q.level) return;
  Q.level = q;
  Q.frames = 0;
  Q.avg = 0;
  if (Q.fn) Q.fn(q, Q.fn_user);
}

void cw_quality_set(cw_quality q, bool user) {
  init_once();
  if (locked() || q < 0 || q >= CW_QUALITY_COUNT) return;
  if (user) Q.user = true;
  change(q);
}

float cw_quality_scale(const char *knob) {
  const preset *p = &P[Q.level];
  if (!knob) return 1;
  if (!strcmp(knob, "particles")) return p->particles;
  if (!strcmp(knob, "density") || !strcmp(knob, "grass") || !strcmp(knob, "crowd") || !strcmp(knob, "scatter")) return p->density;
  return 1;
}
float cw_quality_density(void) { return P[Q.level].density; }
int cw_quality_count(int n, const char *knob) {
  if (n <= 0) return n;
  int k = (int)((float)n * cw_quality_scale(knob) + 0.5f);
  return k < 1 ? 1 : k > n ? n : k;
}

void cw_quality_on_change(cw_quality_change_fn fn, void *user) {
  Q.fn = fn;
  Q.fn_user = user;
}

void cw_quality_auto(bool on, float budget_ms) {
  Q.auto_on = on && !locked();
  if (budget_ms > 0) Q.budget = budget_ms;
}
bool cw_quality_is_auto(void) { return Q.auto_on && !Q.user && !locked(); }

void cw_quality_auto_measure(bool in_play) {
  double now = cw_time();
  double d = (now - Q.last_t) * 1000.0;
  bool was = Q.in_play;
  Q.last_t = now;
  Q.in_play = in_play;
  if (!in_play || !was || !cw_quality_is_auto() || Q.level == CW_QUALITY_LOW) return;
  if (d <= 0 || d > 250) return; /* a hitch (loading, a tab in the background) says nothing */
  /* the first frames of play settle (shaders, uploads) before they count */
  if (++Q.frames <= 30) {
    Q.avg = (float)d;
    return;
  }
  Q.avg += ((float)d - Q.avg) * 0.05f;
  if (Q.frames > 150 && Q.avg > Q.budget) {
    cw_info("quality: %s -> %s (frames averaged %.1f ms, budget %.1f)", NAMES[Q.level], NAMES[Q.level - 1], (double)Q.avg,
            (double)Q.budget);
    change((cw_quality)(Q.level - 1));
  }
}

void cw_quality_cycle(void) {
  init_once();
  if (locked()) return;
  cw_quality_set((cw_quality)((Q.level + 1) % CW_QUALITY_COUNT), true); /* Low, Medium, High, Ultra, Low... */
}

const char *cw_quality_label(void) { return NAMES[Q.level]; }
const char *cw_render_res_label(void) {
  static char buf[16];
  int w = 0, h = 0;
  cw_render_size(&w, &h);
  snprintf(buf, sizeof buf, "%dp", h);
  return buf;
}

uint8_t cw_quality_saved(void) { return Q.user ? (uint8_t)(Q.level + 1) : 0; }
void cw_quality_restore(uint8_t saved) {
  if (saved >= 1 && saved <= CW_QUALITY_COUNT) cw_quality_set((cw_quality)(saved - 1), true);
}

static void cmd_quality(int argc, const char **argv, cw_reply *r) {
  if (argc > 1) {
    if (!strcasecmp(argv[1], "auto")) {
      Q.user = false;
      Q.auto_on = !locked();
    } else {
      int q = -1;
      for (int i = 0; i < CW_QUALITY_COUNT; i++)
        if (!strcasecmp(argv[1], NAMES[i])) q = i;
      if (q < 0) {
        cw_reply_error(r, "quality: low, medium, high, ultra or auto");
        return;
      }
      cw_quality_set((cw_quality)q, true);
    }
  }
  cw_reply_printf(r, "{\"quality\":\"%s\",\"auto\":%s,\"locked\":%s,\"particles\":%g,\"density\":%g}", NAMES[Q.level],
                  cw_quality_is_auto() ? "true" : "false", locked() ? "true" : "false", (double)P[Q.level].particles,
                  (double)P[Q.level].density);
}

/* ── what the renderer asks ───────────────────────────────────────── */

void cw__quality_post(cw_post_settings *s) {
  const preset *p = &P[Q.level];
  if (Q.level == CW_QUALITY_HIGH) return; /* as authored */
  if (s->ssao) {
    if (p->ssao == 0) s->ssao = false;
    else if (p->ssao == 1) {
      s->ssao_slices = 1;
      s->ssao_steps = s->ssao_steps > 4 ? 4 : s->ssao_steps;
    } else if (p->ssao == 3) {
      s->ssao_slices = s->ssao_slices < 3 ? 3 : s->ssao_slices;
      s->ssao_steps = s->ssao_steps < 8 ? 8 : s->ssao_steps;
    }
  }
  if (s->bloom && p->bloom_levels > 0 && s->bloom_levels > p->bloom_levels) s->bloom_levels = p->bloom_levels;
  if (p->aa == 0) s->aa = CW_AA_NONE;
  else if (p->aa == 1 && s->aa == CW_AA_SMAA) s->aa = CW_AA_FXAA;
  else if (p->aa == 3 && s->aa != CW_AA_NONE) s->aa = CW_AA_SMAA;
}

int cw__quality_shadow_size(int authored) {
  const preset *p = &P[Q.level];
  int s = authored * p->shadow_mul;
  if (p->shadow_cap > 0 && s > p->shadow_cap) s = p->shadow_cap;
  return s < 256 ? (authored < 256 ? authored : 256) : s;
}
bool cw__quality_soft_shadows(void) { return P[Q.level].soft_shadows; }

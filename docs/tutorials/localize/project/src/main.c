/* Coin Counter (the localize tutorial): the empty 2D template's game,
 * localized. Every string on screen comes from a catalog (l10n/), the
 * locale switches at runtime, and the a11y settings scale the text and
 * show captions.
 *
 * Input (project.cwproj): ui_accept collects a coin, next_language cycles
 * en -> en-XA -> ar -> ja -> ar-XB -> en, large_text toggles the larger
 * text and the high-contrast theme.
 *
 * Debug fields: game.coins, game.locale (index into LOCALES),
 * game.visited (locales shown so far), text.hash (per locale: a hash of
 * every text the UI shows, from the accessibility tree), text.untranslated
 * (texts shown in en-XA that did not go through a catalog),
 * text.glyphs_missing, text.scale_pct, l10n.* (cw_l10n_debug_register).
 *
 * Env (native, the tutorial's controls): LOCALIZE_TEST_FAULT=missing_key
 * (the hint uses a mistyped key) or =hardcoded (the hint is English text
 * written in the code). */
#include <stdlib.h>
#include <string.h>

#include "cartwheel/a11y.h"
#include "cartwheel/debug.h"
#include "cartwheel/input.h"
#include "cartwheel/l10n.h"
#include "cartwheel/project.h"
#include "cartwheel/render2d.h"
#include "cartwheel/scene2d.h"
#include "cartwheel/scene_file.h"
#include "cartwheel/ui.h"

/* #region locales */
static const char *const LOCALES[] = { "en", "en-XA", "ar", "ja", "ar-XB" };
#define NLOC 5
/* #endregion */

static struct {
  int32_t coins, locale, visited, untranslated, glyphs_missing, scale_pct;
  int32_t hash[NLOC];
  uint32_t seen; /* bit per locale index */
  bool large;
  int intro;
  cw_action collect, next_language, large_text;
} G;

static cw_node hud(const char *path) { return cw_node_get(cw_tree_root(cw_scene_app_tree()), path); }

/* #region refresh */
/* Text the code builds: a plural and a formatted line. Labels whose text is
 * a key ("hud.title") translate themselves; these two are set from code
 * (auto_translate = false in main.cwscene, so they are not translated a
 * second time). Runs on every locale change. */
static void refresh(void *user) {
  (void)user;
  cw_label_set_text(hud("Main/HUD/Panel/Coins"), cw_trn("hud.coins", G.coins));
  cw_label_set_text(hud("Main/HUD/Panel/Language"),
                    cw_trf("hud.language", "name", cw_l10n_locale_name(LOCALES[G.locale]), NULL));
}
/* #endregion */

/* #region set_locale */
static void set_locale(int index) {
  G.locale = index;
  cw_l10n_set_locale(LOCALES[index]); /* loads the locale's @font, mirrors the UI for RTL, calls refresh */
  cw_captions_play(G.intro, (cw_voice){ 0 }, (cw_sound){ 0 }); /* the intro again, in the new language */
}
/* #endregion */

/* #region large_text */
static void set_large_text(bool on) {
  G.large = on;
  cw_ui_set_text_scale(on ? 1.25f : 1.0f);
  cw_ui_set_theme(on ? cw_theme_high_contrast() : (cw_theme){ 0 });
  G.scale_pct = (int32_t)(cw_ui_text_scale() * 100 + 0.5f);
}
/* #endregion */

static void init(void *user) {
  (void)user;
  cw_project_load_input();
  G.collect = cw_action_find("ui_accept");
  G.next_language = cw_action_find("next_language");
  G.large_text = cw_action_find("large_text");
  /* #region load */
  if (!cw_l10n_load_project("l10n/project.cwl10n")) cw_error("coin_counter: l10n/project.cwl10n did not load");
  cw_l10n_debug_register("l10n"); /* l10n.missing, l10n.keys, ... and the `l10n` debug command */
  cw_l10n_on_change(refresh, NULL);
  G.intro = cw_captions_load("captions/intro.cwcap");
  /* #endregion */
  const char *fault = getenv("LOCALIZE_TEST_FAULT");
  if (fault && !strcmp(fault, "missing_key")) cw_label_set_text(hud("Main/HUD/Panel/Hint"), "hud.hnit");
  if (fault && !strcmp(fault, "hardcoded")) cw_label_set_text(hud("Main/HUD/Panel/Hint"), "Press south to collect a coin");
  /* a shipping game starts from cw_l10n_system_locale() (or a saved
   * setting); this one starts in English so a test run is the same on
   * every machine */
  set_locale(0);
  set_large_text(false);
  cw_debug_i32("game.coins", &G.coins);
  cw_debug_i32("game.locale", &G.locale);
  cw_debug_i32("game.visited", &G.visited);
  cw_debug_field("text.hash", G.hash, CW_DBG_I32, NLOC);
  cw_debug_i32("text.untranslated", &G.untranslated);
  cw_debug_i32("text.glyphs_missing", &G.glyphs_missing);
  cw_debug_i32("text.scale_pct", &G.scale_pct);
}

/* #region input */
static void fixed_update(void *user, double dt) {
  (void)user;
  cw_node coin = hud("Main/Coin");
  cw_node_set(coin, "rotation", cw_val_float((double)cw_tick_index() * dt));
  if (cw_action_just_pressed(0, G.collect)) {
    G.coins++;
    refresh(NULL);
    cw_captions_show("caption.coin", "", 1.5f); /* a sound caption: a key, translated when shown */
  }
  if (cw_action_just_pressed(0, G.next_language)) set_locale((G.locale + 1) % NLOC);
  if (cw_action_just_pressed(0, G.large_text)) set_large_text(!G.large);
}
/* #endregion */

/* #region measure */
/* After the UI is drawn: hash every text it shows (the accessibility tree
 * holds what the user sees, translated), count glyphs no font had, and in
 * Japanese count texts with Latin letters in them: in this game those
 * never went through a catalog. */
static bool has_latin(const char *s) {
  for (; *s; s++)
    if ((*s >= 'A' && *s <= 'Z') || (*s >= 'a' && *s <= 'z')) return true;
  return false;
}

static void render(void *user, double alpha) {
  (void)user, (void)alpha;
  cw_tree *t = cw_scene_app_tree();
  int n = cw_a11y_snapshot(t, NULL, 0);
  cw_a11y_node *nodes = cw_frame_alloc(sizeof *nodes * (size_t)(n + 1));
  n = cw_a11y_snapshot(t, nodes, n);
  uint32_t h = CW_HASH32_INIT;
  int untranslated = 0;
  for (int i = 0; i < n; i++) {
    h = cw_hash32(nodes[i].name, strlen(nodes[i].name), h);
    h = cw_hash32(nodes[i].value, strlen(nodes[i].value), h);
    if (has_latin(nodes[i].name) || has_latin(nodes[i].value)) untranslated++;
  }
  G.hash[G.locale] = (int32_t)h;
  if (!strcmp(LOCALES[G.locale], "ja")) G.untranslated = untranslated;
  if (!(G.seen & (1u << G.locale))) G.seen |= 1u << G.locale, G.visited++;
  G.glyphs_missing = (int32_t)cw_r2d_get_stats()->glyphs_missing;
}
/* #endregion */

cw_app_desc cw_main(int argc, char **argv) {
  (void)argc, (void)argv;
  cw_scene_register_builtin_types();
  static cw_app_desc app = { .width = 640, .height = 360, .debug = true, .init = init, .fixed_update = fixed_update,
                             .render = render };
  cw_project_apply_app(&app);
  const char *main_scene = cw_project_get("main_scene");
  return cw_scene_app(main_scene ? main_scene : "main.cwscene", &app);
}

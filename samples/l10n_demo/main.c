/* l10n_demo: one settings-style UI in English, the en-XA pseudo-locale,
 * Arabic (right to left, shaped) and Japanese (CJK glyphs from a fallback
 * font), driven by translation catalogs packed with cwpack (project/).
 *
 * Without input the screen changes every 60 frames so input-free hosts gate
 * each one: en (frame 50), en-XA (110), ar (170), ja (230), ar-XB (290),
 * then English with the accessibility settings on (350): the high-contrast
 * theme, text at 125% and reduced motion. Shown on
 * every screen: catalog texts through the UI's auto-translation, CLDR plural
 * forms (cw_trn over 0 1 2 3 11 100), locale number and date formatting, a
 * wrapped paragraph, a per-locale banner image (cw_l10n_asset_path),
 * captions from a caption track, a RemapButton, a mirrored layout in RTL.
 *
 * L10N_NAV builds (l10n_nav) use the colorblind-friendly theme, fix Arabic
 * and play a scripted gamepad session
 * through the raw input filter (so input-free hosts run it too): focus the
 * language box, open it, choose Japanese, come back to it (focus is
 * restored), move down to the gamepad RemapButton and rebind jump to the
 * west button.
 *
 * Env (native): CW_L10N_LOCALE=<tag> fixes the locale; CW_L10N_FAULT=
 * missing_key | no_fallback | no_font | no_a11y breaks one thing (the gates'
 * controls);
 * CW_L10N_NAV_CONTROL=1 drops the nav script's last press. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cartwheel/gpu.h"
#include "cartwheel/a11y.h"
#include "cartwheel/debug.h"
#include "cartwheel/gl.h"
#include "cartwheel/l10n.h"
#include "cartwheel/scene.h"
#include "cartwheel/ui.h"

static const char *const LOCALES[] = { "en", "en-XA", "ar", "ja", "ar-XB" };

static struct {
  cw_tree *tree;
  cw_node root, title, language, points, stats, paragraph, banner, brand, captions, remap_kb, remap_pad, play;
  int track;
  int locale_index, fixed_locale, segment;
  const char *fault;
  int32_t frame, locale_dbg, text_hash, a11y_nodes, focus_hash, remaps, jump_hash, caption_hash, glyphs_missing,
      shaped_runs, rtl, option_selected, captions_on, a11y_on, text_scale_pct;
  bool nav, nav_control;
} D;

static cw_node node(const char *type, const char *name, cw_node parent) {
  cw_node n = cw_node_new(type, name);
  cw_node_add_child(parent, n);
  return n;
}
static cw_node label(const char *name, const char *text, cw_node parent) {
  cw_node l = node("Label", name, parent);
  cw_label_set_text(l, text);
  return l;
}

/* texts the code builds from the catalogs (labels showing them do not
 * auto-translate again) */
/* #region texts */
static void refresh_texts(void) {
  static const double ns[] = { 0, 1, 2, 3, 11, 100 };
  char buf[1024] = "";
  for (size_t i = 0; i < CW_ARRAY_LEN(ns); i++) {
    if (i) strncat(buf, "  \xC2\xB7  ", sizeof buf - strlen(buf) - 1);
    strncat(buf, cw_trn("score.points", ns[i]), sizeof buf - strlen(buf) - 1);
  }
  cw_label_set_text(D.points, buf);
  cw_label_set_text(D.stats, cw_trf("stats.line", "score", cw_l10n_format_number(1234567.5, 1), "date",
                                    cw_l10n_format_date(2026, 10, 6, CW_DATE_LONG), NULL));
  cw_node_set(D.banner, "texture", cw_val_string(cw_l10n_asset_path("textures/banner.png")));
  /* the language box lists every locale by its own name (the pseudo-locales
 * by a name that says what they are) */
  cw_option_button_clear(D.language);
  for (size_t i = 0; i < CW_ARRAY_LEN(LOCALES); i++) cw_option_button_add_item(D.language, cw_l10n_locale_name(LOCALES[i]));
  cw_option_button_select(D.language, D.locale_index);
}
/* #endregion */

static void set_locale(int index) {
  D.locale_index = index;
  cw_l10n_set_locale(LOCALES[index]);
  if (D.fault && !strcmp(D.fault, "no_font")) cw_font_set_global_fallbacks(NULL, 0); /* control: lose the CJK/Arabic fonts */
  refresh_texts();
  cw_captions_play(D.track, (cw_voice){ 0 }, (cw_sound){ 0 }); /* frame clock: the same on every host */
  D.locale_dbg = index;
}

/* the accessibility screen: high contrast, larger text, reduced motion */
/* #region a11y-settings */
static void set_a11y(bool on) {
  bool skip = D.fault && !strcmp(D.fault, "no_a11y"); /* control: the settings never apply */
  cw_ui_set_theme(on && !skip ? cw_theme_high_contrast() : (cw_theme){ 0 });
  cw_ui_set_text_scale(on && !skip ? 1.25f : 1);
  cw_ui_set_reduced_motion(on && !skip);
  D.a11y_on = on && !skip;
  D.text_scale_pct = (int32_t)(cw_ui_text_scale() * 100 + 0.5f);
}
/* #endregion */

static void on_language(cw_node e, const cw_value *a, int c, void *u) {
  (void)e, (void)c, (void)u;
  if (a[0].i >= 0 && a[0].i < (int64_t)CW_ARRAY_LEN(LOCALES) && a[0].i != D.locale_index) set_locale((int)a[0].i);
}
static void on_subtitles(cw_node e, const cw_value *a, int c, void *u) {
  (void)e, (void)c, (void)u;
  cw_captions_set_enabled(a[0].b);
}
static void on_remapped(cw_node e, const cw_value *a, int c, void *u) {
  (void)e, (void)c, (void)u;
  D.remaps++;
  D.jump_hash = (int32_t)cw_hash32(a[0].s, strlen(a[0].s), CW_HASH32_INIT);
}

/* ── the scripted gamepad session (L10N_NAV) ─────────────────────────── */
typedef struct { int frame; uint32_t buttons; } nav_step;
#define B(x) (1u << CW_PAD_##x)
static const nav_step NAV[] = {
  { 20, B(DOWN) },  /* nothing focused: focus the first control (the language box) */
  { 30, B(SOUTH) },      /* open it */
  { 40, B(DOWN) },       /* the list opens with no item highlighted: en */
  { 46, B(DOWN) },       /* en-XA */
  { 52, B(DOWN) },       /* ar */
  { 58, B(DOWN) },       /* ja */
  { 64, B(SOUTH) },      /* choose: Japanese; focus comes back to the box */
  { 72, B(DOWN) },  /* name field */
  { 80, B(DOWN) },  /* volume */
  { 88, B(RIGHT) }, /* volume up */
  { 96, B(DOWN) },  /* subtitles */
  { 104, B(SOUTH) },     /* subtitles off */
  { 112, B(DOWN) }, /* jump (keyboard) */
  { 120, B(DOWN) }, /* jump (gamepad) */
  { 128, B(SOUTH) },     /* listen */
  { 140, B(WEST) },      /* bind the west button (the control run skips this) */
};

static void autopilot(cw_raw_input *in, uint64_t frame, void *u) {
  (void)u;
  in->pads[0].connected = true;
  for (size_t i = 0; i < CW_ARRAY_LEN(NAV); i++) {
    if (D.nav_control && i == CW_ARRAY_LEN(NAV) - 1) break;
    if ((int)frame == NAV[i].frame || (int)frame == NAV[i].frame + 1) in->pads[0].buttons |= NAV[i].buttons;
    if ((int)frame == NAV[i].frame) in->pads[0].pressed |= NAV[i].buttons;
  }
}

/* ── state for the gates ─────────────────────────────────────────────── */
/* #region snapshot */
static void publish(void) {
  int n = cw_a11y_snapshot(D.tree, NULL, 0);
  cw_a11y_node *nodes = cw_frame_alloc(sizeof *nodes * (size_t)(n + 1));
  n = cw_a11y_snapshot(D.tree, nodes, n);
  uint32_t h = CW_HASH32_INIT;
  for (int i = 0; i < n; i++) {
    h = cw_hash32(nodes[i].name, strlen(nodes[i].name), h);
    h = cw_hash32(nodes[i].value, strlen(nodes[i].value), h);
    h = cw_hash32(&nodes[i].role, sizeof nodes[i].role, h);
  }
  D.text_hash = (int32_t)h;
  D.a11y_nodes = n;
/* #endregion */
  char path[512] = "";
  cw_node f = cw_ui_focus_owner();
  if (cw_node_valid(f)) cw_node_path(f, (cw_node){ 0 }, path, sizeof path);
  D.focus_hash = (int32_t)cw_hash32(path, strlen(path), CW_HASH32_INIT);
  cw_caption_line lines[8];
  int k = cw_captions_active(lines, 8);
  uint32_t ch = CW_HASH32_INIT;
  for (int i = 0; i < k; i++) ch = cw_hash32(lines[i].text, strlen(lines[i].text), ch);
  D.caption_hash = (int32_t)ch;
  const cw_r2d_stats *rs = cw_r2d_get_stats();
  D.glyphs_missing = (int32_t)rs->glyphs_missing;
  D.shaped_runs = (int32_t)rs->shaped_runs;
  D.rtl = cw_l10n_is_rtl();
  D.option_selected = cw_option_button_selected(D.language);
  D.captions_on = cw_button_pressed(cw_node_get(D.root, "Panel/V/Form/Subtitles"));
}

static void init(void *u) {
  (void)u;
  D.fault = getenv("CW_L10N_FAULT");
  D.nav_control = getenv("CW_L10N_NAV_CONTROL") != NULL;
#ifdef L10N_NAV
  D.nav = true;
#endif
  cw_scene_register_builtin_types();
  D.tree = cw_tree_new();
  cw_set_current_tree(D.tree);
  /* #region load */
  if (!cw_l10n_load_project("l10n/project.cwl10n")) cw_error("l10n_demo: the localization project did not load");
  if (D.fault && !strcmp(D.fault, "no_fallback")) cw_l10n_set_default_locale("xx"); /* control: no English to fall back on */
  cw_l10n_debug_register("l10n");
  D.track = cw_captions_load("captions/intro.cwcap");
  /* #endregion */
  cw_action jump = cw_action_add("jump", 0.5f);
  cw_action_bind_text(jump, "key:space, pad:south");
  cw_input_commit_defaults();

  D.root = node("Control", "Root", cw_tree_root(D.tree));
  cw_control_set_anchors_preset(D.root, CW_PRESET_FULL_RECT, 0);
  cw_node panel = node("PanelContainer", "Panel", D.root);
  cw_control_set_anchors_preset(panel, CW_PRESET_FULL_RECT, 14);
  cw_node v = node("VBoxContainer", "V", panel);
  cw_node_set(v, "theme_overrides", cw_val_string("separation = 9"));
  cw_node top = node("HBoxContainer", "Top", v);
  D.title = label("Title", "app.title", top);
  cw_node_set(D.title, "theme_overrides", cw_val_string("font_size = 26"));
  cw_control_set_size_flags(D.title, CW_SIZE_EXPAND_FILL, CW_SIZE_SHRINK_CENTER);
  D.banner = node("TextureRect", "Banner", top);
  cw_node_set(D.banner, "stretch_mode", cw_val_int(4));
  cw_control_set_custom_min_size(D.banner, (cw_vec2){ 200, 38 });
  cw_node_set(D.banner, "accessibility_name", cw_val_string("banner.alt"));
  /* #region form */
  /* the form: label | control rows (a11y names come from the labels) */
  cw_node form = node("GridContainer", "Form", v);
  cw_node_set(form, "columns", cw_val_int(2));
  label("LanguageLabel", "menu.language", form);
  D.language = node("OptionButton", "Language", form);
  cw_control_set_auto_translate(D.language, false); /* locale names are shown as they are */
  cw_control_set_size_flags(D.language, CW_SIZE_EXPAND_FILL, CW_SIZE_FILL);
  cw_connect(D.language, "item_selected", (cw_node){ 0 }, NULL, on_language, NULL, 0);
  label("NameLabel", "form.name", form);
  cw_node name = node("LineEdit", "Name", form);
  cw_node_set(name, "placeholder_text", cw_val_string("form.name_hint"));
  label("VolumeLabel", "form.volume", form);
  cw_node vol = node("HSlider", "Volume", form);
  cw_range_set_limits(vol, 0, 100, 10);
  cw_range_set_value(vol, 70);
  label("SubtitlesLabel", "form.subtitles", form);
  cw_node subs = node("CheckBox", "Subtitles", form);
  cw_button_set_text(subs, "form.subtitles_on");
  cw_button_set_toggle_mode(subs, true);
  cw_button_set_pressed(subs, true);
  cw_connect(subs, "toggled", (cw_node){ 0 }, NULL, on_subtitles, NULL, 0);
  label("JumpLabel", "form.jump", form);
  D.remap_kb = node("RemapButton", "JumpKeys", form);
  cw_node_set(D.remap_kb, "action", cw_val_string("jump"));
  label("JumpPadLabel", "form.jump_pad", form);
  D.remap_pad = node("RemapButton", "JumpPad", form);
  cw_node_set(D.remap_pad, "action", cw_val_string("jump"));
  cw_node_set(D.remap_pad, "slot", cw_val_int(1));
  cw_node_set(D.remap_pad, "device", cw_val_enum("GAMEPAD"));
  /* #endregion */
  cw_connect(D.remap_kb, "remapped", (cw_node){ 0 }, NULL, on_remapped, NULL, 0);
  cw_connect(D.remap_pad, "remapped", (cw_node){ 0 }, NULL, on_remapped, NULL, 0);
  /* text the code formats: plurals, numbers, dates */
  D.points = label("Points", "", v);
  cw_control_set_auto_translate(D.points, false);
  D.stats = label("Stats", "", v);
  cw_control_set_auto_translate(D.stats, false);
  D.paragraph = label("Paragraph", "app.paragraph", v);
  cw_node_set(D.paragraph, "autowrap", cw_val_bool(true));
  cw_node buttons = node("HBoxContainer", "Buttons", v);
  D.play = node("Button", "Play", buttons);
  cw_button_set_text(D.play, "menu.play");
  cw_control_set_tooltip(D.play, "tooltip.play");
  cw_button_set_text(node("Button", "Settings", buttons), "menu.settings");
  cw_button_set_text(node("Button", "Quit", buttons), "menu.quit");
  D.brand = label("Brand", "app.brand", buttons); /* only the English catalog has it: every locale falls back */
  cw_control_set_size_flags(D.brand, CW_SIZE_EXPAND_FILL, CW_SIZE_SHRINK_CENTER);
  cw_node_set(D.brand, "horizontal_alignment", cw_val_enum("RIGHT"));
  if (D.fault && !strcmp(D.fault, "missing_key")) cw_button_set_text(D.play, "menu.plya"); /* control: a typo'd key */
  D.captions = node("CaptionPanel", "Captions", v);
  cw_control_set_size_flags(D.captions, CW_SIZE_EXPAND_FILL, CW_SIZE_EXPAND_FILL);

  const char *fixed = getenv("CW_L10N_LOCALE");
  D.fixed_locale = -1;
#ifdef L10N_NAV
  D.fixed_locale = 2;
#endif
  for (size_t i = 0; fixed && i < CW_ARRAY_LEN(LOCALES); i++)
    if (!strcmp(fixed, LOCALES[i])) D.fixed_locale = (int)i;
  set_locale(D.fixed_locale >= 0 ? D.fixed_locale : 0);
  D.text_scale_pct = 100;
  if (D.nav) {
    cw_ui_set_theme(cw_theme_colorblind());
    cw_set_raw_input_filter(autopilot, NULL);
  }

  cw_debug_i32("demo.frame", &D.frame);
  cw_debug_i32("demo.locale", &D.locale_dbg);
  cw_debug_i32("demo.text_hash", &D.text_hash);
  cw_debug_i32("demo.a11y_nodes", &D.a11y_nodes);
  cw_debug_i32("demo.focus_hash", &D.focus_hash);
  cw_debug_i32("demo.remaps", &D.remaps);
  cw_debug_i32("demo.jump_hash", &D.jump_hash);
  cw_debug_i32("demo.caption_hash", &D.caption_hash);
  cw_debug_i32("demo.glyphs_missing", &D.glyphs_missing);
  cw_debug_i32("demo.shaped_runs", &D.shaped_runs);
  cw_debug_i32("demo.rtl", &D.rtl);
  cw_debug_i32("demo.option_selected", &D.option_selected);
  cw_debug_i32("demo.captions_on", &D.captions_on);
  cw_debug_i32("demo.segment", &D.segment);
  cw_debug_i32("demo.a11y_on", &D.a11y_on);
  cw_debug_i32("demo.text_scale_pct", &D.text_scale_pct);
}

static void update(void *u, double dt) {
  (void)u;
  D.frame++;
  if (D.fixed_locale < 0) {
    int seg = ((D.frame - 1) / 60) % ((int)CW_ARRAY_LEN(LOCALES) + 1);
    if (seg != D.segment) {
      D.segment = seg;
      set_a11y(seg == (int)CW_ARRAY_LEN(LOCALES));
      int want = seg < (int)CW_ARRAY_LEN(LOCALES) ? seg : 0;
      if (want != D.locale_index || seg == 0) set_locale(want);
    }
  }
  cw_tree_update(D.tree, dt);
}

static void render(void *u, double a) {
  (void)u, (void)a;
  int w, h;
  cw_drawable_size(&w, &h);
  (void)w, (void)h;
  cw_gpu_clear_window(0.07f, 0.08f, 0.1f, 1); /* both backends */
  cw_ui_render(D.tree);
  publish();
}

cw_app_desc cw_main(int argc, char **argv) {
  (void)argc, (void)argv;
  return (cw_app_desc){ .title = "l10n demo", .width = 960, .height = 600, .init = init, .update = update,
                        .render = render, .debug = true };
}

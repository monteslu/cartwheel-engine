/* ui_hud: a pixel-art game HUD. The game draws into a 640x360 virtual
 * canvas scaled to the window by whole steps (CW_SCALE_INTEGER); the UI is
 * drawn INSIDE that pass (cw_ui_draw) on its own canvas layer above the
 * game, at UI scale 2, so the HUD is 320x180 UI units and every UI pixel is
 * a 2x2 canvas block, then upscaled again with the game. Pointer input is
 * mapped back through the virtual canvas: the scripted clicks land on the
 * buttons in window coordinates. Unhandled clicks reach the game. */
#include <math.h>
#include <stdio.h>

#include "cartwheel/debug.h"
#include "cartwheel/gl.h"
#include "cartwheel/scene.h"
#include "cartwheel/ui.h"

static struct {
  cw_tree *tree;
  cw_node score, health, pause, menu, root;
  int32_t ticks, paused, menu_presses, game_clicks, ui_wants;
  float health_v;
} H;

static const char *HUD_THEME =
    "extends = dark\n"
    "default_font_size = 9\n"
    "PanelContainer.panel = style(bg=#10141cd0, border=1, border_color=#ffffff40, radius=3, margin=5 3)\n"
    "BoxContainer.separation = 3\n"
    "Button.normal = style(bg=#202838e0, border=1, border_color=#ffffff50, radius=3, margin=6 2)\n"
    "Button.hover = style(base=Button.normal, bg=#2c3850e0)\n"
    "Button.pressed = style(base=Button.normal, bg=#c04040e0, border_color=#ffffffa0)\n"
    "Button.disabled = style(base=Button.normal)\n"
    "Button.focus = style(bg=#00000000, border=1, border_color=#ffd84d, radius=3, draw_center=0)\n"
    "ProgressBar.background = style(bg=#00000080, radius=2)\n"
    "ProgressBar.fill = style(bg=#e8504a, radius=2)\n";

static void s_pause(cw_node e, const cw_value *a, int c, void *u) { (void)e, (void)c, (void)u; H.paused = a[0].b; }
static void s_menu(cw_node e, const cw_value *a, int c, void *u) { (void)e, (void)a, (void)c, (void)u; H.menu_presses++; }
/* #region unhandled */
static void unhandled(const cw_ui_event *e, void *u) {
  (void)u;
  if (e->type == CW_UI_MOUSE_DOWN) H.game_clicks++;
}
/* #endregion */

static cw_node node(const char *type, const char *name, cw_node parent) {
  cw_node n = cw_node_new(type, name);
  cw_node_add_child(parent, n);
  return n;
}

static void init(void *u) {
  (void)u;
  cw_scene_register_builtin_types();
  H.tree = cw_tree_new();
  cw_set_current_tree(H.tree);
  /* #region build */
  cw_ui_set_scale(2);
  H.root = node("Control", "Hud", cw_tree_root(H.tree));
  cw_control_set_anchors_preset(H.root, CW_PRESET_FULL_RECT, 0);
  cw_control_set_mouse_filter(H.root, CW_MOUSE_IGNORE); /* clicks on empty HUD go to the game */
  /* one theme for the whole HUD, written in the theme text format */
  cw_control_set_theme(H.root, cw_theme_parse(HUD_THEME, "hud.theme"));
  cw_node panel = node("PanelContainer", "Stats", H.root);
  cw_control_set_anchors_preset(panel, CW_PRESET_TOP_LEFT, 4);
  cw_node v = node("VBoxContainer", "V", panel);
  H.score = node("Label", "Score", v);
  cw_node_set(H.score, "theme_overrides", cw_val_string("font_size = 9; font_color = #ffd84d"));
  H.health = node("ProgressBar", "Health", v);
  cw_node_set(H.health, "show_percentage", cw_val_bool(false));
  cw_control_set_custom_min_size(H.health, (cw_vec2){ 70, 6 });
  cw_node bar = node("HBoxContainer", "Buttons", H.root);
  cw_control_set_anchors_preset(bar, CW_PRESET_BOTTOM_RIGHT, 4);
  H.pause = node("Button", "Pause", bar);
  cw_button_set_text(H.pause, "Pause");
  cw_button_set_toggle_mode(H.pause, true);
  H.menu = node("Button", "Menu", bar);
  cw_button_set_text(H.menu, "Menu");
  cw_connect(H.pause, "toggled", (cw_node){ 0 }, NULL, s_pause, NULL, 0);
  cw_connect(H.menu, "pressed", (cw_node){ 0 }, NULL, s_menu, NULL, 0);
  /* #endregion */
  cw_ui_set_unhandled_input(unhandled, NULL);
  cw_debug_i32("ticks", &H.ticks);
  cw_debug_i32("paused", &H.paused);
  cw_debug_i32("menu_presses", &H.menu_presses);
  cw_debug_i32("game_clicks", &H.game_clicks);
  cw_debug_i32("ui_wants", &H.ui_wants);
  cw_ui_debug_register("ui");
}

static void fixed_update(void *u, double dt) {
  (void)u, (void)dt;
  if (!H.paused) H.ticks++;
}

static void update(void *u, double dt) {
  (void)u, (void)dt;
  char s[32];
  snprintf(s, sizeof s, "SCORE %05d", H.ticks * 10);
  cw_label_set_text(H.score, s);
  cw_range_set_value(H.health, 100 - (H.ticks % 100));
  H.ui_wants += cw_ui_wants_pointer();
}

static void render(void *u, double a) {
  (void)u, (void)a;
  /* #region draw */
  cw_r2d_begin(&(cw_r2d_pass_desc){ .clear = true, .clear_color = cw_hex(0x2a3550ff), .virtual_width = 640,
                                     .virtual_height = 360, .scale = CW_SCALE_INTEGER, .letterbox = CW_BLACK });
  /* the "game": scrolling ground and bouncing blocks */
  cw_r2d_rect((cw_rect){ 0, 280, 640, 80 }, cw_hex(0x3d6b3aff));
  for (int i = 0; i < 12; i++) {
    float x = fmodf((float)(i * 57 + H.ticks * 2), 680.0f) - 40;
    float y = 240 - fabsf(sinf((float)(H.ticks + i * 9) * 0.05f)) * 60;
    cw_r2d_rect((cw_rect){ floorf(x), floorf(y), 24, 24 }, cw_hex(i & 1 ? 0xf0a040ff : 0x70c0f0ff));
  }
  cw_ui_draw(H.tree);
  cw_r2d_end();
  /* #endregion */
}

cw_app_desc cw_main(int argc, char **argv) {
  (void)argc, (void)argv;
  return (cw_app_desc){ .title = "ui hud", .width = 1280, .height = 720, .init = init, .fixed_update = fixed_update,
                        .update = update, .render = render, .debug = true };
}

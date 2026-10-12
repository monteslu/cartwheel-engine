/* ui_gallery: every widget, loaded from a .cwscene (controls are nodes with
 * reflected properties), in the dark theme or, built as ui_gallery_light,
 * the light one. A menu with a submenu is opened from code so popups are in
 * the frame too. Layout results (rects of a few controls) and the UI stats
 * are debug fields, so hosts are compared on layout as well as pixels. */
#include "cartwheel/debug.h"
#include "cartwheel/gpu.h"
#include "cartwheel/scene_file.h"
#include "cartwheel/ui.h"

#ifndef GALLERY_LIGHT
#define GALLERY_LIGHT 0
#endif

static struct {
  cw_tree *tree;
  cw_node root;
  float rects[6][4];
  int32_t frames;
  int32_t ellipsized; /* the Ellipsis label's text is wider than its box, so it is cut */
} G;

static const char *probes[6] = { "Margin/Rows/Cols/Left/Buttons/Normal", "Margin/Rows/Cols/Left/Paragraph",
                                 "Margin/Rows/Cols/Middle/Tabs", "Margin/Rows/Cols/Right/Picker",
                                 "Margin/Rows/Cols/Left/Fields/Name", "Margin/Rows/Cols/Middle/Split/Items" };

static void init(void *u) {
  (void)u;
  cw_scene_register_builtin_types();
  G.tree = cw_tree_new();
  cw_set_current_tree(G.tree);
  if (GALLERY_LIGHT) cw_ui_set_theme(cw_theme_default_light());
  G.root = cw_scene_instance_path("scenes/gallery.cwscene");
  cw_node_add_child(cw_tree_root(G.tree), G.root);
  /* a real bold face for [b]; italics stay synthetic (no italics_font) */
  cw_node rich = cw_node_get(G.root, "Margin/Rows/Cols/Left/Rich");
  cw_control_override_font(rich, "bold_font", cw_font_load("fonts/FiraSans-Bold.ttf", NULL));
  for (int i = 0; i < 6; i++) cw_debug_f32_array(cw_frame_printf("rect.%d", i), G.rects[i], 4);
  cw_debug_i32("gallery.ellipsized", &G.ellipsized);
  cw_ui_debug_register("ui");
}

static void update(void *u, double dt) {
  (void)u, (void)dt;
  G.frames++;
  if (G.frames == 30) { /* open View, then File > Recent */
    cw_node file = cw_node_get(G.root, "Margin/Rows/Menu/File");
    cw_rect mb = cw_control_global_rect(cw_node_get(G.root, "Margin/Rows/Menu"));
    cw_popup_menu_popup(file, (cw_vec2){ mb.x, mb.y + mb.h });
  }
  for (int i = 0; i < 6; i++) {
    cw_rect r = cw_control_global_rect(cw_node_get(G.root, probes[i]));
    G.rects[i][0] = r.x, G.rects[i][1] = r.y, G.rects[i][2] = r.w, G.rects[i][3] = r.h;
  }
  cw_node el = cw_node_get(G.root, "Margin/Rows/Cols/Left/Ellipsis");
  cw_value text;
  if (cw_node_get_prop(el, "text", &text) && text.s) {
    float tw = cw_ui_text_width(cw_control_theme_font(el, NULL, "font"), cw_control_theme_font_size(el, NULL, "font_size"), text.s);
    G.ellipsized = tw > cw_control_rect(el).w;
  }
}

static void render(void *u, double a) {
  (void)u, (void)a;
  int w, h;
  cw_drawable_size(&w, &h);
  (void)w, (void)h;
  cw_gpu_clear_window(0, 0, 0, 1); /* both backends */
  cw_ui_render(G.tree);
}

cw_app_desc cw_main(int argc, char **argv) {
  (void)argc, (void)argv;
  return (cw_app_desc){ .title = "ui gallery", .width = 1280, .height = 720, .init = init, .update = update,
                        .render = render, .debug = true };
}

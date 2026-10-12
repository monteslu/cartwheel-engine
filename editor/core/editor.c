/* Editor lifecycle: init (core, modules, settings, window), the per-frame
 * work, debug fields for gates and tools. */
#include "internal.h"
#include "three.h"

static cw_tree *ui_tree;
cw_tree *cwed_ui_tree(void) { return ui_tree; }

void cwed__dock_view_menu(void);

static void core_menus(void) {
  cwed_menu_add("File/New Scene", "scene.new", 100);
  cwed_menu_add("File/Open Scene...", "scene.open", 101);
  cwed_menu_add("File/Save Scene", "scene.save", 110);
  cwed_menu_add("File/Save Scene As...", "palette.open text=\"scene.save path=\"", 111);
  cwed_menu_add("File/Save All Scenes", "scene.save_all", 112);
  cwed_menu_add("File/Close Scene", "scene.close", 120);
  if (!CW_CART) cwed_menu_add("File/Open Project...", "project.open", 200);
  cwed_menu_add("File/Project Info", "project.info", 201);
  if (!CW_CART) cwed_menu_add("File/Quit", "editor.quit", 900);
  cwed_menu_add("Edit/Undo", "edit.undo", 100);
  cwed_menu_add("Edit/Redo", "edit.redo", 101);
  cwed_menu_add("Edit/Undo Editor Change", "edit.undo scope=editor", 110);
  cwed_menu_add("Edit/Select All", "select.all", 200);
  cwed_menu_add("Edit/Select None", "select.clear", 201);
  cwed_menu_add("Edit/Command Palette...", "palette.open", 300);
  cwed_menu_add("Edit/Editor Settings...", "settings.open", 900);
  cwed_menu_add("Scene/Add Node...", "node.add", 100);
  cwed_menu_add("Scene/Instance Scene...", "palette.open text=\"node.instance \"", 101);
  cwed_menu_add("Scene/Rename Node...", "node.rename", 110);
  cwed_menu_add("Scene/Delete Node", "node.delete", 111);
  cwed_menu_add("View/Reset Layout", "layout.reset", 900);
  cwed_menu_add("Play/Play Scene", "play.start", 100);
  cwed_menu_add("Play/Pause", "play.pause", 101);
  cwed_menu_add("Play/Step", "play.step", 102);
  cwed_menu_add("Play/Stop", "play.stop", 103);
  cwed_menu_add("Help/All Commands...", "palette.open", 100);
  cwed_toolbar_add("Play", "Play", "play.start", 100);
  cwed_toolbar_add("Pause", "Pause", "play.pause", 110);
  cwed_toolbar_add("Step", "Step", "play.step", 120);
  cwed_toolbar_add("Stop", "Stop", "play.stop", 130);
}

static bool undo_enabled(void) { return cwed_history(cwed_doc_active()).undo_count > 0; }
static bool redo_enabled(void) { return cwed_history(cwed_doc_active()).redo_count > 0; }

/* the editor's three.c program table (editor/programs/cw_editor.txt) */
extern const struct t3_gen_table cw_editor_gl;
#if CW_WGPU
extern const struct t3_gen_table cw_editor_wgpu;
#define CW_EDITOR_WGPU (&cw_editor_wgpu)
#else
#define CW_EDITOR_WGPU NULL
#endif

void cwed_init(int argc, char **argv) {
  static bool programs;
  if (!programs) { /* once a process, before the editor's first render */
    programs = true;
    t3_register_program_table(&cw_editor_gl, CW_EDITOR_WGPU);
  }
  cw_scene_register_builtin_types();
  ui_tree = cw_tree_new();
  cw_set_current_tree(ui_tree);
  cwed__log_init();
  cwed__commands_init();
  cwed__editor_commands();
  cwed__docs_init();
  cwed__node_commands();
  cwed__settings_init();
  cwed__dock_commands();
  cwed__shell_commands();
  cwed__play_commands();
  /* undo/redo gray out with nothing to do */
  cwed_command_desc u = *cwed_command_find("edit.undo"), r = *cwed_command_find("edit.redo");
  u.enabled = undo_enabled;
  r.enabled = redo_enabled;
  cwed_command_register(&u);
  cwed_command_register(&r);
  cwed__project_init(argc, argv);
  core_menus();
  for (int i = 0; i < cwed__module_count; i++)
    if (cwed__modules[i]->init) cwed__modules[i]->init();
  cwed__dock_view_menu();
  cwed__settings_load();
  cwed__theme_apply();
  cwed__settings_apply_all();
  cwed__shell_build();
  cwed__dock_place_all();
  cwed_log(CWED_LOG_INFO, "cartwheel editor: %d commands, %d modules, project %s", cwed_command_count(), cwed__module_count,
           cwed_project_name());
  const char *main_scene = cwed_project_main_scene();
  if (cwed_setting_bool("editor.open_main_scene") && main_scene[0] && cw_vfs_exists(main_scene))
    cwed_runf("scene.open path=%s", cwed_quote(main_scene));
  cwed_history_seal();
}

void cwed_frame(double dt) {
  cwed__log_drain();
  cwed__settings_tick(dt);
  cwed__dock_frame();
  cwed__play_frame();
  /* a pointer release or Enter ends a merge run (a drag, a typed value) */
  const cw_raw_input *in = cw_input_raw();
  static uint32_t prev_buttons;
  if (in) {
    if ((prev_buttons & 1) && !(in->mouse_buttons & 1)) cwed_history_seal();
    if (in->keys_pressed[40] || in->keys_pressed[88]) cwed_history_seal();
    prev_buttons = in->mouse_buttons;
  }
  for (int i = 0; i < cwed__module_count; i++)
    if (cwed__modules[i]->frame) cwed__modules[i]->frame(dt);
  cwed__shell_frame(dt);
}

void cwed_shutdown(void) {
  cwed__settings_flush(true);
  for (int i = cwed__module_count - 1; i >= 0; i--)
    if (cwed__modules[i]->shutdown) cwed__modules[i]->shutdown();
  cwed__docs_shutdown();
}

/* ── debug fields: what gates compare across hosts ─────────────────────── */

static struct {
  int32_t docs, nodes, undo, redo, dirty, selection, modules, commands, panels_closed, palette, log_lines, version;
  uint32_t doc_hash, sel_hash, layout_hash, runs, errors, active_hash;
} F;

static void refresh_fields(const cwed_event *e, void *user) {
  (void)e, (void)user;
  cwed_doc *d = cwed_doc_active();
  F.docs = cwed_doc_count();
  F.nodes = d ? d->node_count : 0;
  F.doc_hash = d ? d->text_hash : 0;
  F.version = d ? (int32_t)d->version : 0;
  cwed_history_info h = cwed_history(d);
  F.undo = d ? h.undo_count : 0;
  F.redo = d ? h.redo_count : 0;
  F.dirty = d && cwed_doc_dirty(d);
  F.selection = cwed_selection_count();
  F.sel_hash = cwed__selection_hash();
  F.layout_hash = cwed__dock_layout_hash();
  F.runs = cwed__commands_run_count();
  F.errors = cwed__commands_error_count();
  F.palette = cwed__palette_open();
  F.commands = cwed_command_count();
  F.modules = cwed__module_count;
  const char *t = d ? cwed_doc_title(d) : "";
  F.active_hash = cw_hash32(t, strlen(t), CW_HASH32_INIT);
}

void cwed__fields_frame(void);
void cwed__fields_frame(void) { refresh_fields(NULL, NULL); }

void cwed_debug_fields(void) {
  cw_debug_i32("editor.docs", &F.docs);
  cw_debug_i32("editor.nodes", &F.nodes);
  cw_debug_u32("editor.doc_hash", &F.doc_hash);
  cw_debug_i32("editor.doc_version", &F.version);
  cw_debug_u32("editor.active_hash", &F.active_hash);
  cw_debug_i32("editor.undo", &F.undo);
  cw_debug_i32("editor.redo", &F.redo);
  cw_debug_i32("editor.dirty", &F.dirty);
  cw_debug_i32("editor.selection", &F.selection);
  cw_debug_u32("editor.sel_hash", &F.sel_hash);
  cw_debug_u32("editor.layout_hash", &F.layout_hash);
  cw_debug_u32("editor.commands_run", &F.runs);
  cw_debug_u32("editor.command_errors", &F.errors);
  cw_debug_i32("editor.palette_open", &F.palette);
  cw_debug_i32("editor.commands", &F.commands);
  cw_debug_i32("editor.modules", &F.modules);
  cwed_subscribe(CWED_EV_ALL, refresh_fields, NULL);
  refresh_fields(NULL, NULL);
}

/* History panel (M4.0): the active scene's undo history as command lines.
 * Clicking an entry undoes or redoes up to it (history.goto). The scope
 * switch shows the editor-wide history (settings) instead. */
#include <stdio.h>
#include <string.h>

#include "cwed/editor.h"

static cw_node list, scope, title;
static bool editor_scope, syncing;

static void rebuild(void) {
  syncing = true;
  cw_item_list_clear(list);
  cwed_doc *d = editor_scope ? NULL : cwed_doc_active();
  if (!editor_scope && !d) {
    cw_label_set_text(title, "No scene open");
    syncing = false;
    return;
  }
  cwed_history_info h = cwed_history(d);
  cw_label_set_text(title, cw_frame_printf("%s: %d done, %d undone", d ? cwed_doc_title(d) : "Editor", h.undo_count, h.redo_count));
  cw_item_list_add_item(list, editor_scope ? "(start)" : "(opened)", (cw_texture){ 0 });
  for (int i = 0;; i++) {
    bool done;
    const char *l = cwed_history_line(d, i, &done);
    if (!l) break;
    /* long lines are cut so the list never widens the dock (history.list has them whole) */
    int len = (int)strlen(l);
    cw_item_list_add_item(list, cw_frame_printf("%s%.*s%s", done ? "" : "(undone)  ", len > 46 ? 44 : len, l, len > 46 ? "..." : ""),
                          (cw_texture){ 0 });
  }
  cw_item_list_select(list, h.undo_count, true);
  cw_item_list_ensure_visible(list, h.undo_count);
  syncing = false;
}

static void on_event(const cwed_event *e, void *user) {
  (void)e, (void)user;
  rebuild();
}

static void on_pick(cw_node n, const cw_value *a, int argc, void *user) {
  (void)n, (void)argc, (void)user;
  if (syncing) return;
  cwed_runf("history.goto index=%d scope=%s", (int)a[0].i, editor_scope ? "editor" : "scene");
}

static void on_scope(cw_node n, const cw_value *a, int argc, void *user) {
  (void)n, (void)argc, (void)user;
  editor_scope = a[0].i == 1;
  rebuild();
}

static cw_node create(void *user) {
  (void)user;
  cw_node box = cw_node_new("VBoxContainer", NULL);
  cw_node row = cw_node_new("HBoxContainer", "Row");
  title = cw_node_new("Label", "Title");
  cw_control_set_size_flags(title, CW_SIZE_EXPAND_FILL, CW_SIZE_SHRINK_CENTER);
  cw_node_set(title, "ellipsis", cw_val_bool(true));
  scope = cw_node_new("OptionButton", "Scope");
  cw_option_button_add_item(scope, "Scene");
  cw_option_button_add_item(scope, "Editor");
  cw_connect(scope, "item_selected", (cw_node){ 0 }, NULL, on_scope, NULL, 0);
  cw_node_add_child(row, title);
  cw_node_add_child(row, scope);
  cw_node_add_child(box, row);
  list = cw_node_new("ItemList", "Entries");
  cw_control_set_size_flags(list, CW_SIZE_EXPAND_FILL, CW_SIZE_EXPAND_FILL);
  cw_connect(list, "item_selected", (cw_node){ 0 }, NULL, on_pick, NULL, 0);
  cw_node_add_child(box, list);
  rebuild();
  return box;
}

static void init(void) {
  cwed_panel_register(&(cwed_panel_desc){ .id = "history", .title = "History", .slot = "left_bottom", .order = 0, .create = create });
  cwed_subscribe(CWED_EV_HISTORY | CWED_EV_DOC_ACTIVE | CWED_EV_DOC_CLOSED, on_event, NULL);
}

CWED_MODULE(history) = { .title = "History", .milestone = "M4.0", .init = init };

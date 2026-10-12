/* Scene Text panel (M4.0): the active scene exactly as it will save
 * (.cwscene text), refreshed on every change. The editor is agent-first:
 * whatever a human does here is visible as the text diff an agent reads. */
#include <stdio.h>
#include <string.h>

#include "cwed/editor.h"

static cw_node text, header;
static bool dirty = true;

static int highlight(cw_node te, const char *line, int len, int idx, cw_text_span *out, int cap, void *user) {
  (void)idx, (void)user;
  int n = 0;
  if (len <= 0 || cap < 3) return 0;
  if (line[0] == '[') { /* section header: [node name="..." ...] */
    out[n++] = (cw_text_span){ 0, len, cw_control_theme_color(te, "TextEdit", "keyword_color") };
    for (int i = 0; i < len && n < cap; i++)
      if (line[i] == '"') {
        int j = i + 1;
        while (j < len && line[j] != '"') j++;
        out[n++] = (cw_text_span){ i, CW_MIN(j + 1, len), cw_control_theme_color(te, "TextEdit", "string_color") };
        i = j;
      }
    return n;
  }
  const char *eq = memchr(line, '=', (size_t)len);
  if (eq) {
    int k = (int)(eq - line);
    out[n++] = (cw_text_span){ 0, k, cw_control_theme_color(te, "TextEdit", "type_color") };
    int v = k + 1;
    while (v < len && line[v] == ' ') v++;
    const char *name = line[v] == '"' ? "string_color" : (line[v] >= 'A' && line[v] <= 'Z') ? "preprocessor_color" : "number_color";
    out[n++] = (cw_text_span){ v, len, cw_control_theme_color(te, "TextEdit", name) };
  }
  return n;
}

static void refresh(void) {
  dirty = false;
  cwed_doc *d = cwed_doc_active();
  if (!d) {
    cw_label_set_text(header, "No scene open: File > New Scene (Ctrl+N) or Open Scene (Ctrl+O)");
    cw_text_edit_set_text(text, "");
    return;
  }
  char *t = cwed_doc_text(d);
  cw_label_set_text(header, cw_frame_printf("%s%s  ·  the scene as it will save (%d bytes)",
                                            cwed_doc_path(d)[0] ? cwed_doc_path(d) : cwed_doc_title(d),
                                            cwed_doc_dirty(d) ? " (unsaved)" : "", t ? (int)strlen(t) : 0));
  cw_text_edit_set_text(text, t ? t : "");
  cw_free(t);
}

static void on_event(const cwed_event *e, void *user) {
  (void)e, (void)user;
  dirty = true;
}

static void frame(double dt) {
  (void)dt;
  if (dirty && cw_node_valid(text)) refresh();
}

static cw_node create(void *user) {
  (void)user;
  cw_node box = cw_node_new("VBoxContainer", NULL);
  header = cw_node_new("Label", "Header");
  cw_node_set(header, "ellipsis", cw_val_bool(true));
  cw_node_set(header, "theme_overrides", cw_val_string("font_size = 13; font_color = #9aa2b1"));
  text = cw_node_new("TextEdit", "Text");
  cw_node_set(text, "editable", cw_val_bool(false));
  cw_node_set(text, "show_line_numbers", cw_val_bool(true));
  cw_text_edit_set_highlighter(text, highlight, NULL);
  cw_control_set_size_flags(text, CW_SIZE_EXPAND_FILL, CW_SIZE_EXPAND_FILL);
  cw_node_add_child(box, header);
  cw_node_add_child(box, text);
  refresh();
  return box;
}

static void init(void) {
  cwed_panel_register(&(cwed_panel_desc){ .id = "scene_text", .title = "Scene Text", .slot = "center", .order = 50, .create = create });
  cwed_subscribe(CWED_EV_DOC_CHANGED | CWED_EV_DOC_ACTIVE | CWED_EV_DOC_CLOSED | CWED_EV_DOC_SAVED | CWED_EV_HISTORY, on_event, NULL);
}

CWED_MODULE(scene_text) = { .title = "Scene Text", .milestone = "M4.0", .init = init, .frame = frame };

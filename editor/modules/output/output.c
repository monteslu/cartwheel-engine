/* Output panel: the editor log (commands run, their errors, engine
 * warnings and errors), newest at the bottom. */
#include <stdio.h>
#include <string.h>

#include "cwed/editor.h"

#define SHOW 300
static cw_node text;
static bool dirty = true;

static int highlight(cw_node te, const char *line, int len, int idx, cw_text_span *out, int cap, void *user) {
  (void)idx, (void)user, (void)cap;
  if (len <= 0) return 0;
  const char *name = !strncmp(line, "error", 5) ? "string_color" : !strncmp(line, "warn", 4) ? "number_color"
                   : !strncmp(line, "> ", 2) ? "type_color" : "comment_color";
  /* error lines in the keyword color stand out most */
  if (!strncmp(line, "error", 5)) name = "keyword_color";
  out[0] = (cw_text_span){ 0, len, cw_control_theme_color(te, "TextEdit", name) };
  return 1;
}

static void refresh(void) {
  dirty = false;
  int n = cwed_log_count(), from = n > SHOW ? n - SHOW : 0;
  size_t cap = 64;
  for (int i = from; i < n; i++) cap += strlen(cwed_log_line(i, NULL)) + 12;
  char *buf = cw_frame_alloc(cap), *w = buf;
  *w = 0;
  for (int i = from; i < n; i++) {
    cwed_log_kind k;
    const char *l = cwed_log_line(i, &k);
    static const char *pre[] = { "", "warn: ", "error: ", "> " };
    w += sprintf(w, "%s%s%s", i > from ? "\n" : "", pre[k], l);
  }
  cw_text_edit_set_text(text, buf);
  int lines = cw_text_edit_line_count(text);
  cw_text_edit_set_caret(text, lines > 0 ? lines - 1 : 0, 0);
}

static void on_log(const cwed_event *e, void *user) {
  (void)e, (void)user;
  dirty = true;
}

static cw_node create(void *user) {
  (void)user;
  text = cw_node_new("TextEdit", NULL);
  cw_node_set(text, "editable", cw_val_bool(false));
  cw_node_set(text, "highlight_current_line", cw_val_bool(false));
  cw_text_edit_set_highlighter(text, highlight, NULL);
  refresh();
  return text;
}

/* the log can change many times a frame: rebuild the text once per frame */
static void frame(double dt) {
  (void)dt;
  if (dirty && cw_node_valid(text)) refresh();
}

static void init(void) {
  cwed_panel_register(&(cwed_panel_desc){ .id = "output", .title = "Output", .slot = "bottom", .order = 0, .create = create });
  cwed_subscribe(CWED_EV_LOG, on_log, NULL);
}

CWED_MODULE(output) = { .title = "Output", .init = init, .frame = frame };

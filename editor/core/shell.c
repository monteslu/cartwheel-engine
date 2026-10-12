/* The editor window: top bar (menus, toolbar), the dock, the document bar,
 * the status bar, the command palette, the settings dialog, shortcuts and
 * the editor theme. */
#include "internal.h"

static cw_node root, menubar, toolbar_box, status_box, palette, pal_edit, pal_list, settings_dlg, settings_grid, project_label;

cw_node cwed__root(void) { return root; }
cw_node cwed__dock_doc_bar(void);

/* ── theme ────────────────────────────────────────────────────────────── */

static const char *THEME =
    "extends = {base}\n"
    "default_font_size = {size}\n"
    "focus = style(bg=#00000000, border=2, border_color={accent}, radius=4, draw_center=0)\n"
    "Panel.panel = style(bg={bg})\n"
    "Button.normal = style(bg={btn}, border=1, border_color={border}, radius=4, margin=10 4)\n"
    "Button.hover = style(base=Button.normal, bg={btn_hover})\n"
    "Button.pressed = style(base=Button.normal, bg={btn_pressed}, border_color={accent})\n"
    "Button.focus = style(bg=#00000000, border=2, border_color={accent}, radius=5, draw_center=0)\n"
    "LineEdit.normal = style(bg={field}, border=1, border_color={border}, radius=4, margin=6 3)\n"
    "LineEdit.focus = style(bg=#00000000, border=2, border_color={accent}, radius=4, draw_center=0)\n"
    "LineEdit.selection_color = {accent}66\n"
    "Tree.selected = style(bg={accent}4d, radius=3)\n"
    "ItemList.selected = style(bg={accent}4d, radius=3)\n"
    "Slider.grabber_area = style(bg={accent}, radius=3, margin=0 3)\n"
    "Slider.grabber_border_color = {accent}\n"
    "TabContainer.panel = style(bg={panel}, border=1, border_color={border}, radius=0 4 4 4, margin=6)\n"
    "TabContainer.tab_selected = style(bg={panel}, border=1 2 1 0, border_color={accent}, radius=4 4 0 0, margin=10 4)\n"
    "TabContainer.tab_unselected = style(bg={tab}, radius=4 4 0 0, margin=10 4)\n"
    "TabContainer.tab_hovered = style(bg={btn_hover}, radius=4 4 0 0, margin=10 4)\n"
    "PopupMenu.hover = style(bg={accent}, radius=4)\n"
    "MenuBar.pressed = style(bg={accent}, radius=4, margin=10 4)\n"
    "CheckBox.box_checked = style(bg={accent}, border=1, border_color={accent}, radius=4)\n"
    "CheckButton.track_on = style(bg={accent}, border=1, border_color={accent}, radius=9)\n"
    "SplitContainer.separation = 6\n"
    "SplitContainer.grabber_hover_color = {accent}\n"
    "DropTarget.highlight = style(bg={accent}33, border=2, border_color={accent}, radius=4)\n"
    "DragPreview.panel = style(bg={tab}, border=1, border_color={accent}, radius=4, margin=8 4)\n"
    "BoxContainer.separation = 4\n"
    "GridContainer.h_separation = 8\n"
    "GridContainer.v_separation = 4\n"
    "TextEdit.font_size = {code_size}\n"
    "EditorBar.panel = style(bg={bar}, border=0 0 0 1, border_color={border}, margin=6 3)\n"
    "EditorStatus.panel = style(bg={bar}, border=0 1 0 0, border_color={border}, margin=8 3)\n"
    "EditorStatus.font_color = {dim}\n"
    "EditorDocTab.normal = style(bg={tab}, radius=4 4 0 0, margin=10 4)\n"
    "EditorDocTab.hover = style(bg={btn_hover}, radius=4 4 0 0, margin=10 4)\n"
    "EditorDocTab.pressed = style(bg={panel}, border=0 2 0 0, border_color={accent}, radius=4 4 0 0, margin=10 4)\n"
    "Inspector.header = style(bg={tab}, radius=3, margin=6 2)\n";

static const char *accent_hex(void) {
  const char *a = cwed_setting("interface.accent");
  if (a && !strcmp(a, "blue")) return "#4c8dff";
  if (a && !strcmp(a, "orange")) return "#e8913a";
  if (a && !strcmp(a, "purple")) return "#9d7cf0";
  return "#2bb5a0";
}

void cwed__theme_apply(void) {
  bool light = cwed_setting("interface.theme") && !strcmp(cwed_setting("interface.theme"), "light");
  int size = (int)cwed_setting_float("interface.font_size");
  if (size < 8) size = 14;
  struct { const char *k, *v; } tok[] = {
    { "{base}", light ? "light" : "dark" }, { "{size}", cw_frame_printf("%d", size) },
    { "{code_size}", cw_frame_printf("%d", size) }, { "{accent}", accent_hex() },
    { "{bg}", light ? "#d9dce2" : "#191c22" }, { "{panel}", light ? "#f3f4f7" : "#23272f" },
    { "{tab}", light ? "#e3e6eb" : "#2b3039" }, { "{bar}", light ? "#e8eaee" : "#1f2329" },
    { "{btn}", light ? "#e9ebef" : "#2d323c" }, { "{btn_hover}", light ? "#dde1e7" : "#363c48" },
    { "{btn_pressed}", light ? "#cfd4dc" : "#22262e" }, { "{border}", light ? "#c3c8d1" : "#3a404c" },
    { "{field}", light ? "#ffffff" : "#16191e" }, { "{dim}", light ? "#5b6270" : "#9aa2b1" } };
  size_t cap = strlen(THEME) * 2 + 256;
  char *out = cw_frame_alloc(cap), *w = out;
  for (const char *s = THEME; *s;) {
    bool hit = false;
    if (*s == '{')
      for (size_t i = 0; i < CW_ARRAY_LEN(tok); i++) {
        size_t l = strlen(tok[i].k);
        if (!strncmp(s, tok[i].k, l)) {
          w += sprintf(w, "%s", tok[i].v);
          s += l;
          hit = true;
          break;
        }
      }
    if (!hit) *w++ = *s++;
  }
  *w = 0;
  static cw_theme current;
  cw_theme t = cw_theme_parse(out, "editor theme");
  if (CW_HANDLE_IS_NULL(t)) return;
  cw_ui_set_theme(t);
  if (!CW_HANDLE_IS_NULL(current)) cw_theme_destroy(current);
  current = t;
}

/* ── shortcuts ────────────────────────────────────────────────────────── */

static const char *key_name(int sc) {
  static char buf[8];
  if (sc >= 4 && sc <= 29) { buf[0] = (char)('a' + sc - 4); buf[1] = 0; return buf; }
  if (sc >= 30 && sc <= 38) { buf[0] = (char)('1' + sc - 30); buf[1] = 0; return buf; }
  if (sc == 39) return "0";
  if (sc >= 58 && sc <= 69) { snprintf(buf, sizeof buf, "f%d", sc - 57); return buf; }
  switch (sc) {
  case 76: return "delete";
  case 54: return "comma";
  case 41: return "escape";
  case 40: return "enter";
  case 44: return "space";
  case 43: return "tab";
  case 42: return "backspace";
  default: return NULL;
  }
}

/* "ctrl+shift+z" -> "Ctrl+Shift+Z" (first of a comma list) */
static const char *pretty_shortcut(const char *s) {
  if (!s || !*s) return NULL;
  char *o = cw_frame_alloc(64), *w = o;
  bool up = true;
  for (; *s && *s != ',' && w - o < 60; s++) {
    *w++ = up && *s >= 'a' && *s <= 'z' ? (char)(*s - 32) : *s;
    up = *s == '+';
  }
  *w = 0;
  return o;
}

static bool shortcut_matches(const char *list, const char *combo) {
  size_t l = strlen(combo);
  for (const char *p = list; p && *p;) {
    const char *e = strchr(p, ',');
    size_t pl = e ? (size_t)(e - p) : strlen(p);
    if (pl == l && !strncmp(p, combo, l)) return true;
    p = e ? e + 1 : NULL;
  }
  return false;
}

/* Run a line from a menu, a button or a shortcut: a bare command whose first
 * argument is a node path acts on the selection; a bare command that needs
 * other arguments opens the palette to type them. */
static const cwed_command_desc *line_cmd(const char *line);
void cwed__run_ui(const char *line);
void cwed__run_ui(const char *line) {
  const cwed_command_desc *d = line_cmd(line);
  bool bare = d && !strchr(line, ' ');
  if (!bare) {
    cwed_run(line, NULL);
    return;
  }
  const char *prefix = d->name;
  int first = 0;
  if (d->params && d->params[0].name && d->params[0].type == CWED_ARG_PATH && !d->params[0].def) {
    if (!cwed_selection_count()) {
      cwed_log(CWED_LOG_WARN, "%s acts on the selected node: select one first", d->name);
      return;
    }
    prefix = cw_frame_printf("%s %s", d->name, cwed_quote(cwed_doc_node_path(cwed_doc_active(), cwed_selection_get(0))));
    first = 1;
  }
  bool needs = false;
  for (int k = first; d->params && d->params[k].name; k++) needs |= !d->params[k].def;
  if (needs) cwed_runf("palette.open text=%s", cwed_quote(cw_frame_printf("%s ", prefix)));
  else cwed_run(prefix, NULL);
}

static void on_unhandled(const cw_ui_event *e, void *user) {
  (void)user;
  if (e->type != CW_UI_KEY || e->repeat) return;
  const char *k = key_name(e->key);
  if (!k) return;
  char combo[48];
  snprintf(combo, sizeof combo, "%s%s%s%s", e->ctrl ? "ctrl+" : "", e->shift ? "shift+" : "", e->alt ? "alt+" : "", k);
  for (int i = 0; i < cwed_command_count(); i++) {
    const cwed_command_desc *d = cwed_command_at(i);
    if (d->shortcut && shortcut_matches(d->shortcut, combo)) {
      cwed__run_ui(d->name);
      return;
    }
  }
}

/* ── menus ────────────────────────────────────────────────────────────── */

typedef struct { const char *menu, *label, *line; int order; int owner; } menu_item;
static bool menus_built, menus_stale; /* items added or withdrawn after the menu bar was built */
static CW_ARRAY(menu_item) menu_items;
typedef struct { cw_node menu; int item_index; const char *line; } menu_slot;
static CW_ARRAY(menu_slot) menu_slots;

void cwed_menu_add(const char *path, const char *line, int order) {
  const char *slash = strchr(path, '/');
  CW_ASSERT(slash, "menu path is Menu/Item (got %s)", path);
  menu_item it = { cw_intern_n(path, (size_t)(slash - path)), cw_intern(slash + 1), cw_intern(line), order, cwed__owner };
  cw_array_push(&menu_items, it);
  if (menus_built) menus_stale = true;
}

static int menu_rank(const char *m) {
  static const char *ORDER[] = { "File", "Edit", "Scene", "Node", "View", "Play", "Help" };
  for (size_t i = 0; i < CW_ARRAY_LEN(ORDER); i++)
    if (!strcmp(ORDER[i], m)) return (int)i;
  return 100;
}

static int cmp_items(const void *a, const void *b) {
  const menu_item *x = a, *y = b;
  int rx = menu_rank(x->menu), ry = menu_rank(y->menu);
  if (rx != ry) return rx - ry;
  int c = strcmp(x->menu, y->menu);
  if (c) return c;
  return x->order - y->order;
}

static void on_menu(cw_node menu, const cw_value *a, int argc, void *user) {
  (void)menu, (void)argc, (void)user;
  int id = (int)a[0].i;
  if (id >= 0 && id < menu_slots.count) cwed__run_ui(menu_slots.data[id].line);
}

static const cwed_command_desc *line_cmd(const char *line) {
  const char *tok[1];
  bool q[1];
  return cwed__tokenize(line, tok, q, 1) ? cwed_command_find(tok[0]) : NULL;
}

static bool line_enabled(const char *line) {
  const cwed_command_desc *d = line_cmd(line);
  if (!d) return false;
  if (d->enabled) return d->enabled();
  if ((d->flags & CWED_CMD_SCENE) && !cwed_doc_active()) return false;
  return true;
}

void cwed__shell_refresh_menus(void) {
  for (int i = 0; i < menu_slots.count; i++)
    cw_popup_menu_set_disabled(menu_slots.data[i].menu, menu_slots.data[i].item_index, !line_enabled(menu_slots.data[i].line));
}

static void build_menus(void) {
  menus_built = true;
  menus_stale = false;
  if (menu_items.count > 1) qsort(menu_items.data, (size_t)menu_items.count, sizeof(menu_item), cmp_items);
  cw_node menu = { 0 };
  const char *cur = NULL;
  int last_group = -1;
  for (int i = 0; i < menu_items.count; i++) {
    menu_item *it = &menu_items.data[i];
    if (it->menu != cur) {
      cur = it->menu;
      menu = cw_node_new("PopupMenu", it->menu);
      cw_node_add_child(menubar, menu);
      cw_connect(menu, "id_pressed", (cw_node){ 0 }, NULL, on_menu, NULL, 0);
      last_group = it->order / 100;
    }
    if (it->order / 100 != last_group) {
      cw_popup_menu_add_separator(menu);
      last_group = it->order / 100;
    }
    int idx = cw_popup_menu_add_item(menu, it->label, menu_slots.count);
    const cwed_command_desc *d = line_cmd(it->line);
    /* the shortcut is shown only when the item runs the bare command */
    if (d && d->shortcut && !strcmp(it->line, d->name)) cw_popup_menu_set_shortcut(menu, idx, pretty_shortcut(d->shortcut));
    cw_array_push(&menu_slots, ((menu_slot){ menu, idx, it->line }));
  }
}

/* ── toolbar and status bar ───────────────────────────────────────────── */

typedef struct { const char *id, *label, *line; int order; cw_node button; int owner; } tool;
static CW_ARRAY(tool) tools;
typedef struct { const char *field; int order; cw_node label; } status_field;
static CW_ARRAY(status_field) fields;

void cwed_toolbar_add(const char *id, const char *label, const char *line, int order) {
  cw_array_push(&tools, ((tool){ cw_intern(id), cw_intern(label), cw_intern(line), order, { 0 }, cwed__owner }));
  if (menus_built) menus_stale = true;
}

static void on_tool(cw_node b, const cw_value *a, int argc, void *user) {
  (void)b, (void)a, (void)argc;
  cwed__run_ui(tools.data[(intptr_t)user].line);
}

static int cmp_tool(const void *a, const void *b) { return ((const tool *)a)->order - ((const tool *)b)->order; }

static void build_toolbar(void) {
  if (tools.count > 1) qsort(tools.data, (size_t)tools.count, sizeof(tool), cmp_tool);
  for (int i = 0; i < tools.count; i++) {
    cw_node b = cw_node_new("Button", tools.data[i].id);
    cw_button_set_text(b, tools.data[i].label);
    const cwed_command_desc *d = line_cmd(tools.data[i].line);
    if (d) cw_control_set_tooltip(b, d->shortcut ? cw_frame_printf("%s (%s)", d->title, pretty_shortcut(d->shortcut)) : d->title);
    cw_node_set(b, "focus_mode", cw_val_int(CW_FOCUS_NONE));
    cw_connect(b, "pressed", (cw_node){ 0 }, NULL, on_tool, (void *)(intptr_t)i, 0);
    cw_node_add_child(toolbar_box, b);
    tools.data[i].button = b;
  }
}

static void refresh_toolbar(void) {
  for (int i = 0; i < tools.count; i++) cw_button_set_disabled(tools.data[i].button, !line_enabled(tools.data[i].line));
}

static int cmp_field(const void *a, const void *b) { return ((const status_field *)a)->order - ((const status_field *)b)->order; }

void cwed_status_set(const char *field, int order, const char *text) {
  field = cw_intern(field);
  for (int i = 0; i < fields.count; i++)
    if (fields.data[i].field == field) {
      cw_label_set_text(fields.data[i].label, text);
      return;
    }
  if (CW_HANDLE_IS_NULL(status_box)) return;
  cw_node l = cw_node_new("Label", field);
  cw_label_set_text(l, text);
  cw_node_set(l, "theme_overrides", cw_val_string("font_size = 13"));
  if (!strcmp(field, "message")) {
    cw_control_set_size_flags(l, CW_SIZE_EXPAND_FILL, CW_SIZE_FILL);
    cw_node_set(l, "ellipsis", cw_val_bool(true));
  }
  cw_array_push(&fields, ((status_field){ field, order, l }));
  qsort(fields.data, (size_t)fields.count, sizeof(status_field), cmp_field);
  for (int i = 0; i < fields.count; i++) {
    cw_node f = fields.data[i].label;
    if (cw_node_valid(cw_node_parent(f))) cw_node_remove_child(status_box, f);
  }
  for (int i = 0; i < fields.count; i++) cw_node_add_child(status_box, fields.data[i].label);
}

static void refresh_status(void) {
  cwed_doc *d = cwed_doc_active();
  if (d) {
    cwed_history_info h = cwed_history(d);
    cwed_status_set("doc", 10, cw_frame_printf("%s%s  ·  %d nodes", cwed_doc_path(d)[0] ? cwed_doc_path(d) : cwed_doc_title(d),
                                               cwed_doc_dirty(d) ? " (unsaved)" : "", d->node_count));
    cwed_status_set("history", 20, cw_frame_printf("undo %d  redo %d", h.undo_count, h.redo_count));
  } else {
    cwed_status_set("doc", 10, "no scene open");
    cwed_status_set("history", 20, "");
  }
  int k = cwed_selection_count();
  cwed_status_set("selection", 30, k == 0 ? "" : k == 1 ? cw_frame_printf("selected: %s", cwed_doc_node_path(d, cwed_selection_get(0)))
                                                        : cw_frame_printf("%d selected", k));
}

/* ── document bar ─────────────────────────────────────────────────────── */

static void on_doc_tab(cw_node b, const cw_value *a, int argc, void *user) {
  (void)b, (void)a, (void)argc;
  cwed_runf("scene.switch doc=%d", (int)(intptr_t)user);
}
static void on_new_doc(cw_node b, const cw_value *a, int argc, void *user) {
  (void)b, (void)a, (void)argc, (void)user;
  cwed_run("scene.new", NULL);
}

static void refresh_doc_bar(void) {
  cw_node bar = cwed__dock_doc_bar();
  if (CW_HANDLE_IS_NULL(bar)) return;
  cwed_clear_children(bar);
  for (int i = 0; i < cwed_doc_count(); i++) {
    cwed_doc *d = cwed_doc_at(i);
    cw_node b = cw_node_new("EditorDocTab", NULL);
    cw_button_set_text(b, cw_frame_printf("%s%s", cwed_doc_title(d), cwed_doc_dirty(d) ? " *" : ""));
    cw_button_set_toggle_mode(b, true);
    cw_button_set_group(b, "editor_docs");
    cw_button_set_pressed(b, d == cwed_doc_active());
    cw_node_set(b, "focus_mode", cw_val_int(CW_FOCUS_NONE));
    cw_control_set_tooltip(b, cwed_doc_path(d)[0] ? cwed_doc_path(d) : "not saved yet");
    cw_connect(b, "pressed", (cw_node){ 0 }, NULL, on_doc_tab, (void *)(intptr_t)i, 0);
    cw_node_add_child(bar, b);
  }
  cw_node plus = cw_node_new("Button", "NewScene");
  cw_button_set_text(plus, "+");
  cw_node_set(plus, "flat", cw_val_bool(true));
  cw_node_set(plus, "focus_mode", cw_val_int(CW_FOCUS_NONE));
  cw_control_set_tooltip(plus, "New Scene (Ctrl+N)");
  cw_connect(plus, "pressed", (cw_node){ 0 }, NULL, on_new_doc, NULL, 0);
  cw_node_add_child(bar, plus);
}

/* ── command palette ──────────────────────────────────────────────────── */

typedef struct { const char *line; bool run_now; } pal_entry;
static CW_ARRAY(pal_entry) pal_entries;

static int score(const char *hay, const char *q) {
  /* case-insensitive: 3 prefix, 2 word start, 1 substring, 0 none */
  size_t ql = strlen(q);
  if (!ql) return 1;
  for (size_t i = 0; hay[i]; i++) {
    size_t k = 0;
    while (k < ql && hay[i + k] && (hay[i + k] | 32) == (q[k] | 32)) k++;
    if (k == ql) return i == 0 ? 3 : (hay[i - 1] == ' ' || hay[i - 1] == '.' || hay[i - 1] == '_') ? 2 : 1;
  }
  return 0;
}

static void pal_fill(const char *text) {
  cw_item_list_clear(pal_list);
  cw_array_clear(&pal_entries);
  /* argument mode: "scene.open <prefix>" lists project scenes */
  if (!strncmp(text, "scene.open ", 11) || !strncmp(text, "node.instance ", 14)) {
    bool inst = text[0] == 'n';
    const char *q = text + (inst ? 14 : 11);
    const char **sc;
    int n = cwed__project_scenes(&sc);
    for (int i = 0; i < n; i++)
      if (score(sc[i], q)) {
        cw_item_list_add_item(pal_list, sc[i], (cw_texture){ 0 });
        cw_array_push(&pal_entries, ((pal_entry){ cw_intern(cw_frame_printf("%s %s", inst ? "node.instance" : "scene.open",
                                                                             cwed_quote(sc[i]))), true }));
      }
  } else {
    const char *q = text;
    for (int pass = 3; pass >= 1; pass--)
      for (int i = 0; i < cwed_command_count(); i++) {
        const cwed_command_desc *d = cwed_command_at(i);
        if (d->flags & CWED_CMD_HIDDEN) continue;
        int s = CW_MAX(score(d->title ? d->title : d->name, q), score(d->name, q));
        if (s != pass) continue;
        const char *sc = pretty_shortcut(d->shortcut);
        cw_item_list_add_item(pal_list, cw_frame_printf("%s   ·   %s%s%s", d->title ? d->title : d->name, d->name, sc ? "   " : "",
                                                        sc ? sc : ""),
                              (cw_texture){ 0 });
        bool needs_args = false;
        /* a first node-path argument comes from the selection (cwed__run_ui) */
        bool sel_path = d->params && d->params[0].name && d->params[0].type == CWED_ARG_PATH && cwed_selection_count();
        for (int k = sel_path ? 1 : 0; d->params && d->params[k].name; k++) needs_args |= !d->params[k].def;
        cw_array_push(&pal_entries, ((pal_entry){ d->name, !needs_args }));
      }
  }
  if (cw_item_list_count(pal_list)) cw_item_list_select(pal_list, 0, true);
}

static void pal_close(void) {
  if (cw_popup_visible(palette)) cw_popup_hide(palette);
}

static void pal_run_entry(int i) {
  if (i < 0 || i >= pal_entries.count) return;
  pal_entry e = pal_entries.data[i];
  if (!e.run_now) { /* needs arguments: type them after the name */
    const char *t = cw_frame_printf("%s ", e.line);
    cw_line_edit_set_text(pal_edit, t);
    cw_line_edit_set_caret(pal_edit, (int)strlen(t));
    pal_fill(t);
    return;
  }
  pal_close();
  cwed__run_ui(e.line);
}

static void pal_submit(const char *text) {
  const char *tok[1];
  bool q[1];
  /* a whole command line typed (name + arguments): run it as is */
  if (strchr(text, ' ') && cwed__tokenize(text, tok, q, 1) && cwed_command_find(tok[0])) {
    bool argmode = !strncmp(text, "scene.open ", 11) || !strncmp(text, "node.instance ", 14);
    if (argmode && cw_item_list_selected(pal_list) >= 0) {
      pal_run_entry(cw_item_list_selected(pal_list));
      return;
    }
    pal_close();
    cwed_run(text, NULL);
    return;
  }
  pal_run_entry(cw_item_list_selected(pal_list));
}

static void on_pal_text(cw_node n, const cw_value *a, int argc, void *user) {
  (void)n, (void)argc, (void)user;
  pal_fill(a[0].s ? a[0].s : "");
}
static void on_pal_submit(cw_node n, const cw_value *a, int argc, void *user) {
  (void)n, (void)argc, (void)user;
  pal_submit(a[0].s ? a[0].s : "");
}
static void on_pal_activate(cw_node n, const cw_value *a, int argc, void *user) {
  (void)n, (void)argc, (void)user;
  pal_run_entry((int)a[0].i);
}

static bool pal_input(cw_node n, void *d, const cw_ui_event *e) {
  (void)n, (void)d;
  if (e->type != CW_UI_KEY) return e->type == CW_UI_MOUSE_DOWN || e->type == CW_UI_MOUSE_UP;
  int k = cw_item_list_count(pal_list), sel = cw_item_list_selected(pal_list);
  if (e->key == 82 || e->key == 81) { /* up / down move the list while typing */
    if (!k) return true;
    sel = CW_CLAMP(sel + (e->key == 81 ? 1 : -1), 0, k - 1);
    cw_item_list_select(pal_list, sel, true);
    cw_item_list_ensure_visible(pal_list, sel);
    return true;
  }
  if (e->key == 41) {
    pal_close();
    return true;
  }
  return false;
}

static cw_node_type palette_type = { .name = "EditorCommandPalette", .parent = "PopupPanel",
                                     .doc = "The editor's command palette (a PopupPanel that steers its list from the keyboard)." };

static bool cmd_palette(cwed_call *c) {
  cw_vec2 cs = cw_ui_canvas_size();
  float w = CW_MIN(640, cs.x - 40);
  const char *t = cwed_arg(c, "text");
  cw_line_edit_set_text(pal_edit, t);
  cw_line_edit_set_caret(pal_edit, (int)strlen(t));
  pal_fill(t);
  cw_popup_show(palette, (cw_rect){ (cs.x - w) * 0.5f, 60, w, 380 }, false);
  cw_control_grab_focus(pal_edit);
  return true;
}
static bool cmd_palette_close(cwed_call *c) {
  (void)c;
  pal_close();
  return true;
}
bool cwed__palette_open(void) { return cw_node_valid(palette) && cw_popup_visible(palette); }

static void build_palette(void) {
  static bool registered;
  if (!registered) {
    registered = true;
    cw_node_type_register(&palette_type);
    cw_ui_class_register("EditorCommandPalette", &(cw_ui_class){ .theme_type = "PopupPanel", .input = pal_input });
  }
  palette = cw_node_new("EditorCommandPalette", "CommandPalette");
  cw_node box = cw_node_new("VBoxContainer", "Box");
  pal_edit = cw_node_new("LineEdit", "Query");
  cw_node_set(pal_edit, "placeholder_text", cw_val_string("Type a command, or a whole command line (node.add Sprite2D)"));
  pal_list = cw_node_new("ItemList", "Results");
  cw_control_set_size_flags(pal_list, CW_SIZE_EXPAND_FILL, CW_SIZE_EXPAND_FILL);
  cw_control_set_custom_min_size(pal_list, (cw_vec2){ 0, 300 });
  cw_node_add_child(box, pal_edit);
  cw_node_add_child(box, pal_list);
  cw_node_add_child(palette, box);
  cw_node_add_child(root, palette);
  cw_connect(pal_edit, "text_changed", (cw_node){ 0 }, NULL, on_pal_text, NULL, 0);
  cw_connect(pal_edit, "text_submitted", (cw_node){ 0 }, NULL, on_pal_submit, NULL, 0);
  cw_connect(pal_list, "item_activated", (cw_node){ 0 }, NULL, on_pal_activate, NULL, 0);
}

/* ── settings dialog ──────────────────────────────────────────────────── */

static bool settings_building;

static void on_setting_bool(cw_node b, const cw_value *a, int argc, void *user) {
  (void)b, (void)argc;
  if (!settings_building) cwed_runf("settings.set key=%s value=%s", (const char *)user, a[0].b ? "true" : "false");
}
static void on_setting_choice(cw_node b, const cw_value *a, int argc, void *user) {
  (void)argc;
  if (settings_building) return;
  const char *choices = cw_node_user(b);
  int want = (int)a[0].i, k = 0;
  for (const char *p = choices; p && *p; k++) {
    const char *e = strchr(p, '|');
    size_t l = e ? (size_t)(e - p) : strlen(p);
    if (k == want) {
      cwed_runf("settings.set key=%s value=%s", (const char *)user, cw_frame_printf("%.*s", (int)l, p));
      return;
    }
    p = e ? e + 1 : NULL;
  }
}
static void on_setting_number(cw_node b, const cw_value *a, int argc, void *user) {
  (void)b, (void)argc;
  if (!settings_building)
    cwed_runf("settings.set key=%s value=%g merge=%s", (const char *)user, a[0].f, cwed_quote(cw_frame_printf("settings:%s", (const char *)user)));
}
static void on_setting_text(cw_node b, const cw_value *a, int argc, void *user) {
  (void)b, (void)argc;
  if (!settings_building) cwed_runf("settings.set key=%s value=%s", (const char *)user, cwed_quote(a[0].s ? a[0].s : ""));
}

static void fill_settings(void) {
  settings_building = true;
  cwed_clear_children(settings_grid);
  for (int i = 0; i < cwed__settings_count(); i++) {
    const cwed_setting_desc *s = cwed__settings_at(i);
    if (s->hidden) continue;
    cw_node l = cw_node_new("Label", NULL);
    cw_label_set_text(l, s->key);
    cw_control_set_tooltip(l, s->doc);
    cw_node_set(l, "mouse_filter", cw_val_int(CW_MOUSE_STOP));
    cw_node_add_child(settings_grid, l);
    const char *v = cwed_setting(s->key);
    cw_node w;
    if (s->type == CWED_ARG_BOOL) {
      w = cw_node_new("CheckBox", NULL);
      cw_button_set_pressed(w, cwed_setting_bool(s->key));
      cw_connect(w, "toggled", (cw_node){ 0 }, NULL, on_setting_bool, (void *)s->key, 0);
    } else if (s->choices) {
      w = cw_node_new("OptionButton", NULL);
      int k = 0, sel = 0;
      for (const char *p = s->choices; p && *p; k++) {
        const char *e = strchr(p, '|');
        size_t ln = e ? (size_t)(e - p) : strlen(p);
        const char *item = cw_frame_printf("%.*s", (int)ln, p);
        cw_option_button_add_item(w, item);
        if (!strcmp(item, v)) sel = k;
        p = e ? e + 1 : NULL;
      }
      cw_option_button_select(w, sel);
      cw_node_set_user(w, (void *)s->choices);
      cw_connect(w, "item_selected", (cw_node){ 0 }, NULL, on_setting_choice, (void *)s->key, 0);
    } else if (s->type == CWED_ARG_INT || s->type == CWED_ARG_FLOAT) {
      w = cw_node_new("SpinBox", NULL);
      cw_range_set_limits(w, s->min < s->max ? s->min : -1e6, s->min < s->max ? s->max : 1e6, s->type == CWED_ARG_INT ? 1 : 0.05);
      cw_range_set_value(w, strtod(v, NULL));
      cw_connect(w, "value_changed", (cw_node){ 0 }, NULL, on_setting_number, (void *)s->key, 0);
    } else {
      w = cw_node_new("LineEdit", NULL);
      cw_line_edit_set_text(w, v);
      cw_connect(w, "text_submitted", (cw_node){ 0 }, NULL, on_setting_text, (void *)s->key, 0);
    }
    cw_control_set_custom_min_size(w, (cw_vec2){ 180, 0 });
    cw_node_add_child(settings_grid, w);
  }
  settings_building = false;
}

static bool cmd_settings_open(cwed_call *c) {
  (void)c;
  fill_settings();
  cw_dialog_popup_centered(settings_dlg);
  return true;
}

static void on_settings_event(const cwed_event *e, void *user) {
  (void)user, (void)e;
  if (cw_popup_visible(settings_dlg)) fill_settings();
}

static void build_settings_dialog(void) {
  settings_dlg = cw_node_new("AcceptDialog", "SettingsDialog");
  cw_node_set(settings_dlg, "title", cw_val_string("Editor Settings"));
  cw_node_set(settings_dlg, "ok_text", cw_val_string("Close"));
  settings_grid = cw_node_new("GridContainer", "Grid");
  cw_node_set(settings_grid, "columns", cw_val_int(2));
  cw_node_add_child(settings_dlg, settings_grid);
  cw_node_add_child(root, settings_dlg);
  cwed_subscribe(CWED_EV_SETTINGS, on_settings_event, NULL);
}

/* ── events that refresh the chrome ───────────────────────────────────── */

static bool chrome_dirty = true;
static void on_any(const cwed_event *e, void *user) {
  (void)user;
  if (e->type == CWED_EV_LOG) {
    cwed_log_kind k;
    int n = cwed_log_count();
    const char *l = n ? cwed_log_line(n - 1, &k) : NULL;
    if (l && (k == CWED_LOG_ERROR || k == CWED_LOG_COMMAND || k == CWED_LOG_WARN))
      cwed_status_set("message", 40, cw_frame_printf("%s%s", k == CWED_LOG_ERROR ? "error: " : k == CWED_LOG_WARN ? "warning: " : "", l));
    return;
  }
  chrome_dirty = true;
}

static void on_project(const cwed_event *e, void *user) {
  (void)e, (void)user;
  if (cw_node_valid(project_label)) cw_label_set_text(project_label, cwed_project_name());
}

/* ── build ────────────────────────────────────────────────────────────── */

/* editor chrome types: plain widgets with their own theme type, so the
 * editor theme styles them without touching every Button / PanelContainer */
static cw_node_type doc_tab_type = { .name = "EditorDocTab", .parent = "Button", .doc = "A document tab of the editor." };
static cw_node_type bar_type = { .name = "EditorBar", .parent = "PanelContainer", .doc = "The editor top bar." };
static cw_node_type status_type = { .name = "EditorStatus", .parent = "PanelContainer", .doc = "The editor status bar." };

static void register_chrome_types(void) {
  static bool done;
  if (done) return;
  done = true;
  cw_node_type_register(&doc_tab_type);
  cw_ui_class_register("EditorDocTab", &(cw_ui_class){ .theme_type = "EditorDocTab" });
  cw_node_type_register(&bar_type);
  cw_ui_class_register("EditorBar", &(cw_ui_class){ .theme_type = "EditorBar" });
  cw_node_type_register(&status_type);
  cw_ui_class_register("EditorStatus", &(cw_ui_class){ .theme_type = "EditorStatus" });
}

void cwed__shell_build(void) {
  register_chrome_types();
  root = cw_node_new("Panel", "Editor");
  cw_control_set_anchors_preset(root, CW_PRESET_FULL_RECT, 0);
  cw_node_add_child(cw_tree_root(cwed_ui_tree()), root);
  cw_node shell = cw_node_new("VBoxContainer", "Shell");
  cw_control_set_anchors_preset(shell, CW_PRESET_FULL_RECT, 0);
  cw_node_set(shell, "theme_overrides", cw_val_string("separation = 0"));
  cw_node_add_child(root, shell);
  /* top bar */
  cw_node top = cw_node_new("EditorBar", "TopBar");
  cw_node row = cw_node_new("HBoxContainer", "Row");
  menubar = cw_node_new("MenuBar", "Menus");
  cw_node spacer = cw_node_new("Control", "Spacer");
  cw_control_set_size_flags(spacer, CW_SIZE_EXPAND_FILL, CW_SIZE_FILL);
  project_label = cw_node_new("Label", "Project");
  cw_label_set_text(project_label, cwed_project_name());
  cw_node_set(project_label, "theme_overrides", cw_val_string("font_color = #8a93a3"));
  toolbar_box = cw_node_new("HBoxContainer", "Toolbar");
  cw_node_add_child(row, menubar);
  cw_node_add_child(row, spacer);
  cw_node_add_child(row, project_label);
  cw_node_add_child(row, toolbar_box);
  cw_node_add_child(top, row);
  cw_node_add_child(shell, top);
  /* dock */
  cw_node body = cw_node_new("MarginContainer", "Body");
  cw_node_set(body, "theme_overrides", cw_val_string("margin_left = 4; margin_top = 4; margin_right = 4; margin_bottom = 4"));
  cw_control_set_size_flags(body, CW_SIZE_EXPAND_FILL, CW_SIZE_EXPAND_FILL);
  cw_node_add_child(shell, body);
  cwed__dock_build(body);
  /* status bar */
  cw_node status = cw_node_new("EditorStatus", "StatusBar");
  status_box = cw_node_new("HBoxContainer", "Fields");
  cw_node_set(status_box, "theme_overrides", cw_val_string("separation = 24"));
  cw_node_add_child(status, status_box);
  cw_node_add_child(shell, status);
  build_menus();
  build_toolbar();
  build_palette();
  build_settings_dialog();
  cwed_status_set("message", 40, "ready: Ctrl+Shift+P runs any command");
  cw_ui_set_unhandled_input(on_unhandled, NULL);
  cwed_subscribe(CWED_EV_ALL & ~CWED_EV_PROJECT, on_any, NULL);
  cwed_subscribe(CWED_EV_PROJECT, on_project, NULL);
}

/* ── registration owners (plugins): rebuild the menu bar and toolbar ────── */

static void rebuild_bars(void) {
  cwed_clear_children(menubar);
  menu_slots.count = 0;
  build_menus();
  cwed_clear_children(toolbar_box);
  build_toolbar();
  chrome_dirty = true;
}

void cwed__shell_owned(int owner, cwed_owner_stats *s) {
  for (int i = 0; i < menu_items.count; i++) s->menu_items += menu_items.data[i].owner == owner;
  for (int i = 0; i < tools.count; i++) s->toolbar_buttons += tools.data[i].owner == owner;
}

void cwed__shell_remove_owner(int owner) {
  int w = 0;
  for (int i = 0; i < menu_items.count; i++)
    if (menu_items.data[i].owner != owner) menu_items.data[w++] = menu_items.data[i];
  bool changed = w != menu_items.count;
  menu_items.count = w;
  w = 0;
  for (int i = 0; i < tools.count; i++)
    if (tools.data[i].owner != owner) tools.data[w++] = tools.data[i];
  changed |= w != tools.count;
  tools.count = w;
  if (changed && menus_built) rebuild_bars();
}

void cwed__shell_frame(double dt) {
  (void)dt;
  if (menus_stale) rebuild_bars();
  if (!chrome_dirty) return;
  chrome_dirty = false;
  refresh_status();
  refresh_doc_bar();
  refresh_toolbar();
  cwed__shell_refresh_menus();
}

void cwed__shell_commands(void) {
  static const cwed_param pal_p[] = { { "text", CWED_ARG_STRING, "", "start with this typed (a command name + space: argument mode)" }, { 0 } };
  cwed_command_register(&(cwed_command_desc){ .name = "palette.open", .title = "Command Palette", .params = pal_p,
    .shortcut = "ctrl+shift+p,f1", .doc = "Search every command by name or title and run it; typing a whole line runs it as is.",
    .exec = cmd_palette });
  cwed_command_register(&(cwed_command_desc){ .name = "palette.close", .title = "Close Command Palette", .flags = CWED_CMD_HIDDEN,
    .doc = "Close the palette.", .exec = cmd_palette_close });
  cwed_command_register(&(cwed_command_desc){ .name = "settings.open", .title = "Editor Settings...", .shortcut = "ctrl+comma",
    .doc = "Show the settings dialog (every visible setting, edited through settings.set).", .exec = cmd_settings_open });
}

/* ── utilities ────────────────────────────────────────────────────────── */

void cwed_clear_children(cw_node n) {
  while (cw_node_child_count(n)) {
    cw_node ch = cw_node_child(n, cw_node_child_count(n) - 1);
    cw_node_remove_child(n, ch);
    cw_node_free(ch);
  }
}

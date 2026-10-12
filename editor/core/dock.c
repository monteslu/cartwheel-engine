/* The dock: six tabbed slots in resizable splits; panels move between them
 * by dragging tabs (or panel.move) and the layout persists in the settings.
 *
 *   Main (HSplit)
 *   ├─ Left (VSplit): left_top / left_bottom
 *   └─ CenterRight (HSplit)
 *      ├─ Middle (VSplit): CenterBox (document bar + center) / bottom
 *      └─ Right (VSplit): right_top / right_bottom */
#include "internal.h"

enum { S_LEFT_TOP, S_LEFT_BOTTOM, S_CENTER, S_BOTTOM, S_RIGHT_TOP, S_RIGHT_BOTTOM, S_COUNT };
static const char *const SLOT_NAMES[S_COUNT] = { "left_top", "left_bottom", "center", "bottom", "right_top", "right_bottom" };

typedef struct {
  cwed_panel_desc d;
  cw_node content;
  int default_slot;
  bool closed;
  int closed_slot; /* where it was when closed */
  int owner;       /* registration owner (owners.c): 0 = the editor, else a plugin */
} panel;

static CW_ARRAY(panel) panels;
static cw_node tabs[S_COUNT], placeholder[S_COUNT];
static cw_node main_split, left_col, center_right, middle, right_col, center_box, doc_bar;
static bool built, placed, show_all;

typedef struct { const char *key; cw_node *split; } split_key;
static const split_key SPLITS[] = {
  { "layout.split.left", &main_split }, { "layout.split.right", &center_right }, { "layout.split.bottom", &middle },
  { "layout.split.left_v", &left_col }, { "layout.split.right_v", &right_col } };

int cwed_slot_count(void) { return S_COUNT; }
const char *cwed_slot_name(int i) { return i >= 0 && i < S_COUNT ? SLOT_NAMES[i] : NULL; }

static int slot_index(const char *name) {
  for (int i = 0; i < S_COUNT; i++)
    if (!strcmp(SLOT_NAMES[i], name)) return i;
  return -1;
}

static panel *panel_by_id(const char *id) {
  for (int i = 0; i < panels.count; i++)
    if (!strcmp(panels.data[i].d.id, id)) return &panels.data[i];
  return NULL;
}

static panel *panel_by_page(cw_node page) {
  for (int i = 0; i < panels.count; i++)
    if (panels.data[i].content.id == page.id) return &panels.data[i];
  return NULL;
}

static int slot_of_page(cw_node page) {
  cw_node p = cw_node_parent(page);
  for (int i = 0; i < S_COUNT; i++)
    if (p.id == tabs[i].id) return i;
  return -1;
}

static void place_late(panel *p);

void cwed_panel_register(const cwed_panel_desc *d) {
  CW_ASSERT(d && d->id && d->title && d->create, "a panel needs id, title and create");
  if (panel_by_id(d->id)) {
    cwed_log(CWED_LOG_ERROR, "%s: panel id '%s' is taken; panel not added", cwed_owner_name(cwed__owner), d->id);
    return;
  }
  int s = slot_index(d->slot ? d->slot : "");
  if (s < 0) {
    cw_warn("panel %s: unknown slot '%s' (left_top left_bottom center bottom right_top right_bottom); using right_top", d->id,
            d->slot ? d->slot : "");
    s = S_RIGHT_TOP;
  }
  panel p = { *d, { 0 }, s, false, s, cwed__owner };
  cw_array_push(&panels, p);
  /* after startup (a plugin loaded or reloaded): build and dock it now */
  if (placed) place_late(&panels.data[panels.count - 1]);
}

cw_node cwed_panel_content(const char *id) {
  panel *p = panel_by_id(id);
  return p ? p->content : (cw_node){ 0 };
}
bool cwed_panel_visible(const char *id) {
  panel *p = panel_by_id(id);
  return p && !p->closed && slot_of_page(p->content) >= 0;
}

/* ── building ─────────────────────────────────────────────────────────── */

static cw_node mk(const char *type, const char *name) { return cw_node_new(type, name); }

static bool tab_can_drop(cw_node target, const cw_ui_drag *d, cw_vec2 local, cw_rect *hl, void *user);
static void tab_drop(cw_node target, const cw_ui_drag *d, cw_vec2 local, void *user);

static void on_split_dragged(cw_node e, const cw_value *a, int argc, void *user) {
  (void)argc, (void)e;
  const split_key *k = user;
  cwed_setting_put(k->key, cw_frame_printf("%g", a[0].f));
}

void cwed__dock_build(cw_node parent) {
  main_split = mk("HSplitContainer", "Main");
  cw_control_set_size_flags(main_split, CW_SIZE_EXPAND_FILL, CW_SIZE_EXPAND_FILL);
  left_col = mk("VSplitContainer", "Left");
  cw_control_set_custom_min_size(left_col, (cw_vec2){ 300, 0 });
  cw_control_set_size_flags(left_col, CW_SIZE_FILL, CW_SIZE_FILL);
  center_right = mk("HSplitContainer", "CenterRight");
  cw_control_set_size_flags(center_right, CW_SIZE_EXPAND_FILL, CW_SIZE_FILL);
  middle = mk("VSplitContainer", "Middle");
  cw_control_set_size_flags(middle, CW_SIZE_EXPAND_FILL, CW_SIZE_FILL);
  right_col = mk("VSplitContainer", "Right");
  cw_control_set_custom_min_size(right_col, (cw_vec2){ 300, 0 });
  cw_control_set_size_flags(right_col, CW_SIZE_FILL, CW_SIZE_FILL);
  center_box = mk("VBoxContainer", "CenterBox");
  cw_control_set_size_flags(center_box, CW_SIZE_FILL, CW_SIZE_EXPAND_FILL);
  cw_node_set(center_box, "theme_overrides", cw_val_string("separation = 0"));
  doc_bar = mk("HBoxContainer", "DocBar");
  cw_node_add_child(center_box, doc_bar);
  for (int i = 0; i < S_COUNT; i++) {
    tabs[i] = mk("TabContainer", SLOT_NAMES[i]);
    cw_node_set(tabs[i], "drag_to_rearrange", cw_val_bool(true));
    cw_ui_set_drop_target(tabs[i], &(cw_ui_drop_fns){ .can_drop = tab_can_drop, .drop = tab_drop, .user = (void *)(intptr_t)i });
    cw_control_set_size_flags(tabs[i], CW_SIZE_EXPAND_FILL, CW_SIZE_EXPAND_FILL);
  }
  cw_control_set_custom_min_size(tabs[S_BOTTOM], (cw_vec2){ 0, 170 });
  cw_control_set_size_flags(tabs[S_BOTTOM], CW_SIZE_FILL, CW_SIZE_FILL);
  cw_node_add_child(left_col, tabs[S_LEFT_TOP]);
  cw_node_add_child(left_col, tabs[S_LEFT_BOTTOM]);
  cw_node_add_child(center_box, tabs[S_CENTER]);
  cw_node_add_child(middle, center_box);
  cw_node_add_child(middle, tabs[S_BOTTOM]);
  cw_node_add_child(right_col, tabs[S_RIGHT_TOP]);
  cw_node_add_child(right_col, tabs[S_RIGHT_BOTTOM]);
  cw_node_add_child(center_right, middle);
  cw_node_add_child(center_right, right_col);
  cw_node_add_child(main_split, left_col);
  cw_node_add_child(main_split, center_right);
  cw_node_add_child(parent, main_split);
  for (size_t i = 0; i < CW_ARRAY_LEN(SPLITS); i++)
    cw_connect(*SPLITS[i].split, "dragged", (cw_node){ 0 }, NULL, on_split_dragged, (void *)&SPLITS[i], 0);
  built = true;
}

cw_node cwed__dock_doc_bar(void);
cw_node cwed__dock_doc_bar(void) { return doc_bar; }

/* ── placement ────────────────────────────────────────────────────────── */

static void refresh_visibility(void) {
  bool vis[S_COUNT];
  for (int i = 0; i < S_COUNT; i++) {
    vis[i] = i == S_CENTER || show_all || cw_tab_container_page_count(tabs[i]) > 0;
    cw_control_set_visible(tabs[i], vis[i]);
  }
  cw_control_set_visible(left_col, vis[S_LEFT_TOP] || vis[S_LEFT_BOTTOM]);
  cw_control_set_visible(right_col, vis[S_RIGHT_TOP] || vis[S_RIGHT_BOTTOM]);
}

static void layout_save(void) {
  char buf[2048];
  int n = 0;
  buf[0] = 0;
  for (int s = 0; s < S_COUNT; s++) {
    int k = cw_tab_container_page_count(tabs[s]);
    bool any = false;
    for (int i = 0; i < k; i++) {
      panel *p = panel_by_page(cw_tab_container_page(tabs[s], i));
      if (!p) continue;
      n += snprintf(buf + n, sizeof buf - (size_t)n, "%s%s%s", any ? "," : (n ? "|" : ""), any ? "" : SLOT_NAMES[s], any ? "" : ":");
      n += snprintf(buf + n, sizeof buf - (size_t)n, "%s", p->d.id);
      any = true;
    }
  }
  bool anyc = false;
  for (int i = 0; i < panels.count; i++)
    if (panels.data[i].closed) {
      n += snprintf(buf + n, sizeof buf - (size_t)n, "%s%s", anyc ? "," : (n ? "|closed:" : "closed:"), panels.data[i].d.id);
      anyc = true;
    }
  cwed_setting_put("layout.panels", buf);
}

static void put_page(panel *p, int slot, int index) {
  cw_node old = cw_node_parent(p->content);
  if (cw_node_valid(old)) cw_node_remove_child(old, p->content);
  int k = cw_node_child_count(tabs[slot]);
  if (index < 0 || index >= k) cw_node_add_child(tabs[slot], p->content);
  else cw_node_add_child_at(tabs[slot], p->content, index);
  p->closed = false;
  for (int i = 0; i < cw_tab_container_page_count(tabs[slot]); i++)
    if (cw_tab_container_page(tabs[slot], i).id == p->content.id) cw_tab_container_set_current(tabs[slot], i);
}

static int cmp_panel(const void *a, const void *b) {
  const panel *x = a, *y = b;
  if (x->default_slot != y->default_slot) return x->default_slot - y->default_slot;
  if (x->d.order != y->d.order) return x->d.order - y->d.order;
  return strcmp(x->d.id, y->d.id);
}

static void place_defaults(void) {
  for (int i = 0; i < panels.count; i++) put_page(&panels.data[i], panels.data[i].default_slot, -1);
  for (int s = 0; s < S_COUNT; s++) cw_tab_container_set_current(tabs[s], 0);
}

static void apply_saved_layout(const char *text) {
  /* "left_top:a,b|bottom:c|closed:d" */
  const char *p = text;
  while (p && *p) {
    const char *bar = strchr(p, '|');
    size_t l = bar ? (size_t)(bar - p) : strlen(p);
    char part[1024];
    snprintf(part, sizeof part, "%.*s", (int)l, p);
    p = bar ? bar + 1 : NULL;
    char *colon = strchr(part, ':');
    if (!colon) continue;
    *colon = 0;
    bool closed = !strcmp(part, "closed");
    int s = slot_index(part);
    if (!closed && s < 0) continue;
    for (char *id = strtok(colon + 1, ","); id; id = strtok(NULL, ",")) {
      panel *pn = panel_by_id(id);
      if (!pn) continue;
      if (closed) {
        int was = slot_of_page(pn->content);
        if (cw_node_valid(cw_node_parent(pn->content))) cw_node_remove_child(cw_node_parent(pn->content), pn->content);
        pn->closed = true;
        pn->closed_slot = was >= 0 ? was : pn->default_slot;
      } else put_page(pn, s, -1);
    }
  }
  for (int s = 0; s < S_COUNT; s++) cw_tab_container_set_current(tabs[s], 0);
}

/* A panel registered after startup: placed where the saved layout has its
 * id (the user's choice survives a plugin reload), else its default slot. */
static void place_late(panel *p) {
  p->content = p->d.create(p->d.user);
  CW_ASSERT(cw_is_control(p->content), "panel %s: create must return a Control", p->d.id);
  cw_node_set_name(p->content, p->d.title);
  cw_control_set_size_flags(p->content, CW_SIZE_EXPAND_FILL, CW_SIZE_EXPAND_FILL);
  int slot = p->default_slot;
  bool closed = false;
  const char *saved = cwed_setting("layout.panels");
  for (const char *q = saved ? saved : ""; *q;) {
    const char *bar = strchr(q, '|');
    size_t l = bar ? (size_t)(bar - q) : strlen(q);
    char part[1024];
    snprintf(part, sizeof part, "%.*s", (int)l, q);
    q = bar ? bar + 1 : q + l;
    char *colon = strchr(part, ':');
    if (!colon) continue;
    *colon = 0;
    for (char *id = strtok(colon + 1, ","); id; id = strtok(NULL, ","))
      if (!strcmp(id, p->d.id)) {
        if (!strcmp(part, "closed")) closed = true;
        else if (slot_index(part) >= 0) slot = slot_index(part);
      }
  }
  if (cw_node_valid(placeholder[slot])) {
    cw_node_free(placeholder[slot]);
    placeholder[slot] = (cw_node){ 0 };
  }
  int keep = cw_tab_container_current(tabs[slot]);
  put_page(p, slot, -1);
  if (keep >= 0) cw_tab_container_set_current(tabs[slot], keep);
  if (closed) {
    cw_node_remove_child(tabs[slot], p->content);
    p->closed = true;
    p->closed_slot = slot;
  }
  cwed_menu_add(cw_frame_printf("View/%s", p->d.title), cw_frame_printf("panel.toggle id=%s", p->d.id), 100 + panels.count);
  refresh_visibility();
}

void cwed__dock_owned(int owner, cwed_owner_stats *s) {
  for (int i = 0; i < panels.count; i++) s->panels += panels.data[i].owner == owner;
}

void cwed__dock_remove_owner(int owner) {
  int w = 0;
  bool any = false;
  for (int i = 0; i < panels.count; i++) {
    panel *p = &panels.data[i];
    if (p->owner != owner) {
      panels.data[w++] = *p;
      continue;
    }
    any = true;
    if (cw_node_valid(p->content)) {
      cw_node par = cw_node_parent(p->content);
      if (cw_node_valid(par)) cw_node_remove_child(par, p->content);
      cw_node_free(p->content);
    }
  }
  panels.count = w;
  if (any && placed) refresh_visibility();
}

static void apply_splits(void) {
  for (size_t i = 0; i < CW_ARRAY_LEN(SPLITS); i++) {
    const char *v = cwed_setting(SPLITS[i].key);
    cw_split_set_offset(*SPLITS[i].split, v && *v ? (float)strtod(v, NULL) : 0);
  }
}

void cwed__dock_place_all(void) {
  if (panels.count > 1) qsort(panels.data, (size_t)panels.count, sizeof(panel), cmp_panel);
  for (int i = 0; i < panels.count; i++) {
    panel *p = &panels.data[i];
    p->content = p->d.create(p->d.user);
    CW_ASSERT(cw_is_control(p->content), "panel %s: create must return a Control", p->d.id);
    cw_node_set_name(p->content, p->d.title);
    cw_control_set_size_flags(p->content, CW_SIZE_EXPAND_FILL, CW_SIZE_EXPAND_FILL);
  }
  placed = true;
  place_defaults();
  const char *saved = cwed_setting("layout.panels");
  if (saved && *saved) apply_saved_layout(saved);
  apply_splits();
  refresh_visibility();
}

/* ── drag and drop of tabs ────────────────────────────────────────────── */

static bool is_slot(cw_node n) {
  for (int i = 0; i < S_COUNT; i++)
    if (tabs[i].id == n.id) return true;
  return false;
}

static panel *dragged_panel(const cw_ui_drag *d) {
  if (strcmp(d->kind, "tab") || !is_slot(d->source)) return NULL;
  return panel_by_page(cw_tab_container_page(d->source, (int)d->payload));
}

static int drop_index(cw_node target, cw_vec2 local) {
  int at = cw_tab_container_tab_at(target, local);
  if (at < 0) return -1;
  cw_rect r = cw_tab_container_tab_rect(target, at);
  return local.x > r.x + r.w * 0.5f ? at + 1 : at;
}

static bool tab_can_drop(cw_node target, const cw_ui_drag *d, cw_vec2 local, cw_rect *hl, void *user) {
  (void)user;
  if (!dragged_panel(d)) return false;
  int at = cw_tab_container_tab_at(target, local);
  if (at >= 0) { /* on the tab bar: a marker where it will land */
    int idx = drop_index(target, local);
    int k = cw_tab_container_page_count(target);
    cw_rect r = idx < k ? cw_tab_container_tab_rect(target, idx) : cw_tab_container_tab_rect(target, k - 1);
    float x = idx < k ? r.x - 2 : r.x + r.w;
    *hl = (cw_rect){ x, 0, 4, r.h };
  }
  return true;
}

static void tab_drop(cw_node target, const cw_ui_drag *d, cw_vec2 local, void *user) {
  int slot = (int)(intptr_t)user;
  panel *p = dragged_panel(d);
  if (!p) return;
  int idx = drop_index(target, local);
  /* a placeholder page does not count */
  if (cw_node_valid(placeholder[slot]) && idx > 0) idx--;
  if (cw_node_parent(p->content).id == target.id && idx > cw_node_index(p->content)) idx--;
  cwed_runf("panel.move id=%s slot=%s index=%d", p->d.id, SLOT_NAMES[slot], idx);
}

void cwed__dock_frame(void) {
  if (!built) return;
  const cw_ui_drag *d = cw_ui_drag_current();
  bool want = d && dragged_panel(d);
  if (want == show_all) return;
  show_all = want;
  for (int s = 0; s < S_COUNT; s++) {
    if (show_all && s != S_CENTER && cw_tab_container_page_count(tabs[s]) == 0) {
      placeholder[s] = mk("Label", "Drop here");
      cw_label_set_text(placeholder[s], "Drop a panel here");
      cw_node_set(placeholder[s], "horizontal_alignment", cw_val_int(CW_HALIGN_CENTER));
      cw_node_set(placeholder[s], "vertical_alignment", cw_val_int(CW_VALIGN_CENTER));
      cw_node_add_child(tabs[s], placeholder[s]);
    } else if (!show_all && cw_node_valid(placeholder[s])) {
      cw_node_free(placeholder[s]);
      placeholder[s] = (cw_node){ 0 };
    }
  }
  refresh_visibility();
}

uint32_t cwed__dock_layout_hash(void) {
  const char *l = cwed_setting("layout.panels");
  uint32_t h = cw_hash32(l ? l : "", l ? strlen(l) : 0, CW_HASH32_INIT);
  for (size_t i = 0; i < CW_ARRAY_LEN(SPLITS); i++) {
    float o = built ? cw_split_offset(*SPLITS[i].split) : 0;
    h = cw_hash32(&o, sizeof o, h);
  }
  return h;
}

/* ── commands ─────────────────────────────────────────────────────────── */

static bool cmd_move(cwed_call *c) {
  panel *p = panel_by_id(cwed_arg(c, "id"));
  if (!p) return cwed_fail(c, "no panel '%s' (panel.list shows them)", cwed_arg(c, "id"));
  int s = slot_index(cwed_arg(c, "slot"));
  if (s < 0) return cwed_fail(c, "no slot '%s' (left_top left_bottom center bottom right_top right_bottom)", cwed_arg(c, "slot"));
  int index = (int)cwed_arg_int(c, "index");
  if (cw_node_valid(placeholder[s])) {
    cw_node_free(placeholder[s]);
    placeholder[s] = (cw_node){ 0 };
  }
  put_page(p, s, index);
  refresh_visibility();
  layout_save();
  cwed_json_key(cwed_reply(c), "slot");
  cwed_json_str(cwed_reply(c), SLOT_NAMES[s]);
  cwed_json_printf(cwed_reply(c), ",\"index\":%d", cw_node_index(p->content));
  return true;
}

static bool cmd_show(cwed_call *c) {
  panel *p = panel_by_id(cwed_arg(c, "id"));
  if (!p) return cwed_fail(c, "no panel '%s' (panel.list shows them)", cwed_arg(c, "id"));
  int s = slot_of_page(p->content);
  if (s < 0) put_page(p, p->closed_slot, -1);
  else put_page(p, s, cw_node_index(p->content));
  refresh_visibility();
  layout_save();
  return true;
}

static bool cmd_close(cwed_call *c) {
  panel *p = panel_by_id(cwed_arg(c, "id"));
  if (!p) return cwed_fail(c, "no panel '%s' (panel.list shows them)", cwed_arg(c, "id"));
  if (p->closed) return true;
  int s = slot_of_page(p->content);
  p->closed_slot = s >= 0 ? s : p->default_slot;
  if (cw_node_valid(cw_node_parent(p->content))) cw_node_remove_child(cw_node_parent(p->content), p->content);
  p->closed = true;
  refresh_visibility();
  layout_save();
  return true;
}

static bool cmd_toggle(cwed_call *c) {
  panel *p = panel_by_id(cwed_arg(c, "id"));
  if (!p) return cwed_fail(c, "no panel '%s' (panel.list shows them)", cwed_arg(c, "id"));
  return p->closed ? cmd_show(c) : cmd_close(c);
}

static bool cmd_list(cwed_call *c) {
  cwed_json *j = cwed_reply(c);
  cwed_json_key(j, "panels");
  cwed_json_raw(j, "[");
  for (int i = 0; i < panels.count; i++) {
    panel *p = &panels.data[i];
    int s = slot_of_page(p->content);
    cwed_json_printf(j, "%s{\"id\":\"%s\",\"title\":", i ? "," : "", p->d.id);
    cwed_json_str(j, p->d.title);
    cwed_json_printf(j, ",\"slot\":\"%s\",\"index\":%d,\"closed\":%s,\"current\":%s}", s >= 0 ? SLOT_NAMES[s] : "",
                     s >= 0 ? cw_node_index(p->content) : -1, p->closed ? "true" : "false",
                     s >= 0 && cw_tab_container_current(tabs[s]) == cw_node_index(p->content) ? "true" : "false");
  }
  cwed_json_raw(j, "]");
  return true;
}

static void rect_json(cwed_json *j, cw_rect r) { cwed_json_printf(j, "[%g,%g,%g,%g]", r.x, r.y, r.w, r.h); }

static bool cmd_info(cwed_call *c) {
  cw_ui_layout(cwed_ui_tree());
  cwed_json *j = cwed_reply(c);
  cwed_json_key(j, "scale");
  cwed_json_printf(j, "%g", cw_ui_scale());
  cwed_json_key(j, "slots");
  cwed_json_raw(j, "{");
  for (int s = 0; s < S_COUNT; s++) {
    cwed_json_key(j, SLOT_NAMES[s]);
    cwed_json_raw(j, "{\"visible\":");
    cwed_json_raw(j, cw_control_visible_in_tree(tabs[s]) ? "true" : "false");
    cwed_json_raw(j, ",\"rect\":");
    rect_json(j, cw_control_global_rect(tabs[s]));
    cwed_json_raw(j, ",\"tabs\":[");
    cw_rect g = cw_control_global_rect(tabs[s]);
    for (int i = 0; i < cw_tab_container_page_count(tabs[s]); i++) {
      cw_rect r = cw_tab_container_tab_rect(tabs[s], i);
      panel *p = panel_by_page(cw_tab_container_page(tabs[s], i));
      cwed_json_printf(j, "%s{\"id\":\"%s\",\"rect\":", i ? "," : "", p ? p->d.id : "");
      rect_json(j, (cw_rect){ g.x + r.x, g.y + r.y, r.w, r.h });
      cwed_json_raw(j, "}");
    }
    cwed_json_raw(j, "]}");
  }
  cwed_json_raw(j, "}");
  cwed_json_key(j, "splits");
  cwed_json_raw(j, "{");
  for (size_t i = 0; i < CW_ARRAY_LEN(SPLITS); i++) {
    cwed_json_key(j, SPLITS[i].key + 13);
    cwed_json_printf(j, "{\"offset\":%g,\"rect\":", cw_split_offset(*SPLITS[i].split));
    rect_json(j, cw_control_global_rect(*SPLITS[i].split));
    cwed_json_raw(j, "}");
  }
  cwed_json_raw(j, "}");
  return true;
}

static bool cmd_split(cwed_call *c) {
  const char *name = cwed_arg(c, "name");
  for (size_t i = 0; i < CW_ARRAY_LEN(SPLITS); i++)
    if (!strcmp(SPLITS[i].key + 13, name)) {
      float o = (float)cwed_arg_float(c, "offset");
      cw_split_set_offset(*SPLITS[i].split, o);
      cwed_setting_put(SPLITS[i].key, cw_frame_printf("%g", o));
      return true;
    }
  return cwed_fail(c, "no split '%s' (left right bottom left_v right_v)", name);
}

static bool cmd_reset(cwed_call *c) {
  (void)c;
  place_defaults();
  for (size_t i = 0; i < CW_ARRAY_LEN(SPLITS); i++) cwed_setting_put(SPLITS[i].key, "0");
  apply_splits();
  refresh_visibility();
  layout_save();
  return true;
}

void cwed__dock_commands(void) {
  static const cwed_param move_p[] = {
    { "id", CWED_ARG_STRING, NULL, "panel id (panel.list)" },
    { "slot", CWED_ARG_STRING, NULL, "left_top left_bottom center bottom right_top right_bottom" },
    { "index", CWED_ARG_INT, "-1", "tab position in the slot (-1 = last)" },
    { 0 } };
  static const cwed_param id_p[] = { { "id", CWED_ARG_STRING, NULL, "panel id (panel.list)" }, { 0 } };
  static const cwed_param split_p[] = {
    { "name", CWED_ARG_STRING, NULL, "left right bottom left_v right_v" },
    { "offset", CWED_ARG_FLOAT, NULL, "separator offset from its natural spot, UI units" },
    { 0 } };
  cwed_command_register(&(cwed_command_desc){ .name = "panel.move", .title = "Move Panel", .params = move_p, .flags = CWED_CMD_HIDDEN,
    .doc = "Dock a panel in a slot (what dragging its tab does). The layout persists.", .exec = cmd_move });
  cwed_command_register(&(cwed_command_desc){ .name = "panel.show", .title = "Show Panel", .params = id_p, .flags = CWED_CMD_HIDDEN,
    .doc = "Bring a panel to the front of its slot (reopening it if closed).", .exec = cmd_show });
  cwed_command_register(&(cwed_command_desc){ .name = "panel.close", .title = "Close Panel", .params = id_p, .flags = CWED_CMD_HIDDEN,
    .doc = "Take a panel out of the dock (panel.show brings it back).", .exec = cmd_close });
  cwed_command_register(&(cwed_command_desc){ .name = "panel.toggle", .title = "Toggle Panel", .params = id_p, .flags = CWED_CMD_HIDDEN,
    .doc = "Close an open panel or reopen a closed one.", .exec = cmd_toggle });
  cwed_command_register(&(cwed_command_desc){ .name = "panel.list", .title = "List Panels", .flags = CWED_CMD_QUERY | CWED_CMD_HIDDEN,
    .doc = "Every panel: id, title, slot, tab index, closed, current.", .exec = cmd_list });
  cwed_command_register(&(cwed_command_desc){ .name = "layout.info", .title = "Layout Info", .flags = CWED_CMD_QUERY | CWED_CMD_HIDDEN,
    .doc = "Slot and tab rects (UI units, window origin) and split offsets: where to point to drag a tab or a split.",
    .exec = cmd_info });
  cwed_command_register(&(cwed_command_desc){ .name = "layout.split", .title = "Set Split", .params = split_p, .flags = CWED_CMD_HIDDEN,
    .doc = "Move a dock separator (what dragging it does).", .exec = cmd_split });
  cwed_command_register(&(cwed_command_desc){ .name = "layout.reset", .title = "Reset Layout",
    .doc = "Put every panel back in its default slot and the separators at their natural spots.", .exec = cmd_reset });
}

/* View menu: one toggle per registered panel (called once modules registered theirs) */
void cwed__dock_view_menu(void);
void cwed__dock_view_menu(void) {
  if (panels.count > 1) qsort(panels.data, (size_t)panels.count, sizeof(panel), cmp_panel);
  for (int i = 0; i < panels.count; i++) {
    cwed_owner_push(panels.data[i].owner); /* the menu item goes when the panel's plugin does */
    cwed_menu_add(cw_frame_printf("View/%s", panels.data[i].d.title), cw_frame_printf("panel.toggle id=%s", panels.data[i].d.id), 100 + i);
    cwed_owner_pop();
  }
}

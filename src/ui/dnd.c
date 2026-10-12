/* Drag and drop: a drag started by a control (cw_ui_drag_begin) follows the
 * pointer until the button comes up; drop targets registered per node accept
 * or refuse it (cw_ui_set_drop_target). */
#include "ui_internal.h"

typedef struct { cw_node node; cw_ui_drop_fns fns; } drop_reg;
static CW_ARRAY(drop_reg) targets;

static drop_reg *find_target(cw_node n) {
  for (int i = 0; i < targets.count; i++)
    if (targets.data[i].node.id == n.id) return &targets.data[i];
  return NULL;
}

void cw_ui_set_drop_target(cw_node n, const cw_ui_drop_fns *fns) {
  /* drop entries of freed nodes while we are here */
  for (int i = targets.count - 1; i >= 0; i--)
    if (!cw_node_valid(targets.data[i].node)) cw_array_remove_swap(&targets, i);
  drop_reg *r = find_target(n);
  if (!fns) {
    if (r) *r = targets.data[--targets.count];
    return;
  }
  if (r) r->fns = *fns;
  else cw_array_push(&targets, ((drop_reg){ n, *fns }));
}

bool cw_ui_drag_begin(const cw_ui_drag *d) {
  if (UI.drag_active || !d || !(UI.buttons & 1)) return false;
  UI.drag = *d;
  UI.drag.kind = cw_intern(d->kind ? d->kind : "");
  snprintf(UI.drag_text, sizeof UI.drag_text, "%s", d->text ? d->text : "");
  snprintf(UI.drag_label, sizeof UI.drag_label, "%s", d->label ? d->label : "");
  UI.drag.text = UI.drag_text;
  UI.drag.label = UI.drag_label;
  UI.drag_active = true;
  UI.drop_target = (cw_node){ 0 };
  UI.tip_shown = false;
  return true;
}

void cw_ui_drag_cancel(void) {
  /* the source still gets its release, marked canceled, like after a drop */
  if (UI.drag_active && (UI.buttons & 1u)) UI.drag_cancel_release = true;
  UI.drag_active = false;
  UI.drop_target = (cw_node){ 0 };
}

const cw_ui_drag *cw_ui_drag_current(void) { return UI.drag_active ? &UI.drag : NULL; }
cw_node cw_ui_drop_target(void) { return UI.drag_active ? UI.drop_target : (cw_node){ 0 }; }

void ui_dnd_update(cw_node under, cw_vec2 p) {
  UI.drop_target = (cw_node){ 0 };
  for (cw_node n = under; cw_node_valid(n); n = ui_parent_control(n)) {
    drop_reg *r = find_target(n);
    if (!r || !r->fns.can_drop) continue;
    cw_rect g = cw_control_global_rect(n);
    cw_rect hl = { 0, 0, g.w, g.h };
    if (r->fns.can_drop(n, &UI.drag, (cw_vec2){ p.x - g.x, p.y - g.y }, &hl, r->fns.user)) {
      UI.drop_target = n;
      UI.drop_highlight = (cw_rect){ g.x + hl.x, g.y + hl.y, hl.w, hl.h };
      return;
    }
  }
}

void ui_dnd_release(void) {
  if (!UI.drag_active) return;
  cw_node t = UI.drop_target;
  cw_ui_drag d = UI.drag;
  UI.drag_active = false;
  UI.drop_target = (cw_node){ 0 };
  if (!cw_node_valid(t)) return;
  drop_reg *r = find_target(t);
  if (!r || !r->fns.drop) return;
  cw_rect g = cw_control_global_rect(t);
  UI.drops++;
  r->fns.drop(t, &d, (cw_vec2){ UI.pointer.x - g.x, UI.pointer.y - g.y }, r->fns.user);
}

void ui_dnd_draw(ui_tree_state *s) {
  if (!UI.drag_active || !cw_node_valid(UI.drag.source) || ui_state_of(UI.drag.source) != s) return;
  cw_node src = UI.drag.source;
  ui_draw_set_origin((cw_vec2){ 0, 0 });
  if (cw_node_valid(UI.drop_target)) {
    cw_ui_style hl = cw_control_theme_style(UI.drop_target, "DropTarget", "highlight");
    cw_ui_draw_style(&hl, UI.drop_highlight);
  }
  if (!UI.drag_label[0]) return;
  cw_ui_style st = cw_control_theme_style(src, "DragPreview", "panel");
  cw_font f = cw_control_theme_font(src, "DragPreview", "font");
  float fs = cw_control_theme_font_size(src, "DragPreview", "font_size");
  float w = cw_ui_text_width(f, fs, UI.drag_label) + SM_W(&st), h = cw_ui_line_height(f, fs) + SM_H(&st);
  cw_vec2 cs = cw_ui_canvas_size();
  float x = CW_MIN(UI.pointer.x + 12, cs.x - w), y = CW_MIN(UI.pointer.y + 14, cs.y - h);
  cw_ui_draw_style(&st, (cw_rect){ x, y, w, h });
  cw_ui_draw_text(f, fs, UI.drag_label, (cw_vec2){ x + st.margin[0], y + st.margin[1] },
                  cw_control_theme_color(src, "DragPreview", "font_color"));
}

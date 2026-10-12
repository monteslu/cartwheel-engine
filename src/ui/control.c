/* Control: the base UI node type, classes, dirty tracking and the two-pass
 * layout (measure, arrange). */
#include <time.h>
#include "ui_internal.h"
#include "cartwheel/l10n.h"
#include "cartwheel/scene2d.h"

ui_system UI;
const cw_node_type *ui_control_t;

/* ── classes ──────────────────────────────────────────────────────────── */

#define UI_MAX_CLASSES 128
static struct { const char *type; cw_ui_class cls; } classes[UI_MAX_CLASSES];
static int class_count;
/* per node type index; each vt is its own allocation because controls keep a
 * pointer to theirs: types (and classes) registered after controls exist
 * must not move them */
static cw__ui_vt **vts;
static bool *vt_built;
static int vt_cap;

void cw_ui_class_register(const char *node_type, const cw_ui_class *cls) {
  const char *t = cw_intern(node_type);
  for (int i = 0; i < class_count; i++)
    if (classes[i].type == t) {
      classes[i].cls = *cls;
      goto rebuilt;
    }
  CW_ASSERT(class_count < UI_MAX_CLASSES, "too many UI classes");
  classes[class_count].type = t;
  classes[class_count++].cls = *cls;
rebuilt:
  if (vt_built) memset(vt_built, 0, sizeof(bool) * (size_t)vt_cap); /* derived types re-resolve */
}

static const cw_ui_class *class_of(const cw_node_type *t) {
  for (int i = 0; i < class_count; i++)
    if (classes[i].type == t->name) return &classes[i].cls;
  return NULL;
}

const cw__ui_vt *ui_vt_for(const cw_node_type *t) {
  if (t->index >= vt_cap) {
    int nc = t->index + 64;
    vts = cw_realloc(vts, sizeof *vts * (size_t)nc, CW_MEM_UI);
    memset(vts + vt_cap, 0, sizeof *vts * (size_t)(nc - vt_cap));
    vt_built = cw_realloc(vt_built, sizeof(bool) * (size_t)nc, CW_MEM_UI);
    memset(vt_built + vt_cap, 0, sizeof(bool) * (size_t)(nc - vt_cap));
    vt_cap = nc;
  }
  if (!vts[t->index]) vts[t->index] = cw_calloc(1, sizeof(cw__ui_vt), CW_MEM_UI);
  cw__ui_vt *v = vts[t->index];
  if (vt_built[t->index]) return v;
  memset(v, 0, sizeof *v);
  v->type = t;
  for (const cw_node_type *c = t; c; c = c->base) {
    const cw_ui_class *k = class_of(c);
    if (!k) continue;
    if (v->ntheme_types < UI_MAX_THEME_TYPES) {
      const char *tt = cw_intern(k->theme_type ? k->theme_type : c->name);
      bool dup = false;
      for (int i = 0; i < v->ntheme_types; i++) dup |= v->theme_types[i] == tt;
      if (!dup) v->theme_types[v->ntheme_types++] = tt;
    }
#define TAKE(m) if (!v->m && k->m) { v->m = k->m; v->m##_t = c; }
    TAKE(min_size) TAKE(layout) TAKE(draw) TAKE(draw_over) TAKE(input) TAKE(has_point) TAKE(focus_changed)
    TAKE(child_offset) TAKE(children_point)
#undef TAKE
    v->wants_text |= k->wants_text;
  }
  v->is_container = v->layout != NULL;
  vt_built[t->index] = true;
  return v;
}

/* ── per-tree state ─────────────────────────────────────────────────── */

static void state_free(void *p) {
  ui_tree_state *s = p;
  cw_array_free(&s->roots);
  cw_array_free(&s->popups);
  cw_free(s);
}

ui_tree_state *ui_state(cw_tree *t) {
  if (!t) return NULL;
  ui_tree_state *s = cw_tree_slot(t, "ui");
  if (!s) {
    s = cw_calloc(1, sizeof *s, CW_MEM_UI);
    s->tree = t;
    s->roots_dirty = true;
    cw_tree_set_slot(t, "ui", s, state_free);
  }
  return s;
}

ui_tree_state *ui_state_of(cw_node n) {
  for (int guard = 0; guard < 64 && cw_node_valid(n); guard++) {
    cw_tree *t = cw_node_tree(n);
    if (t) return ui_state(t);
    cw_control *c = CTL(n);
    cw_node p = cw_node_parent(n);
    if (CW_HANDLE_IS_NULL(p) && c && (c->_flags & CF_INTERNAL)) p = c->_owner;
    n = p;
  }
  return NULL;
}

/* ── helpers ────────────────────────────────────────────────────────── */

cw_control *cw_control_of(cw_node n) { return ui_control_t ? CTL(n) : NULL; }
bool cw_is_control(cw_node n) { return cw_control_of(n) != NULL; }

cw_node ui_parent_control(cw_node n) {
  cw_control *c = CTL(n);
  if (!c || (c->_flags & CF_TOP_LEVEL)) return (cw_node){ 0 };
  cw_node p = cw_node_parent(n);
  return CTL(p) ? p : (cw_node){ 0 };
}

bool ui_is_top_level(cw_node n) { return CW_HANDLE_IS_NULL(ui_parent_control(n)); }

cw_vec2 ui_child_offset(cw_node n) {
  cw_control *c = CTL(n);
  if (!c || !c->_vt->child_offset) return (cw_vec2){ 0, 0 };
  return c->_vt->child_offset(n, cw_node_data_t(n, c->_vt->child_offset_t));
}

static void emit_args(cw_node n, const char *sig, const cw_value *a, int argc) {
  cw_emit(n, sig, a, argc);
  ui_a11y_value_changed(n, sig);
}
void ui_emit0(cw_node n, const char *signal) { emit_args(n, signal, NULL, 0); }
void ui_emit_int(cw_node n, const char *signal, int64_t v) {
  cw_value a = cw_val_int(v);
  emit_args(n, signal, &a, 1);
}
void ui_emit_bool(cw_node n, const char *signal, bool v) {
  cw_value a = cw_val_bool(v);
  emit_args(n, signal, &a, 1);
}
void ui_emit_float(cw_node n, const char *signal, double v) {
  cw_value a = cw_val_float(v);
  emit_args(n, signal, &a, 1);
}
void ui_emit_str(cw_node n, const char *signal, const char *s) {
  cw_value a = cw_val_string(s ? s : "");
  emit_args(n, signal, &a, 1);
}

/* ── dirty tracking ─────────────────────────────────────────────────── */

void ui_mark_min_dirty(cw_node n) {
  /* a min size change can change every ancestor's min size and placement:
   * mark the chain (depth is small; clean siblings are skipped on arrange) */
  for (int guard = 0; guard < 256; guard++) {
    cw_control *c = CTL(n);
    if (!c) return;
    c->_flags |= CF_MIN_DIRTY | CF_LAYOUT_DIRTY;
    if (c->_flags & CF_TOP_LEVEL) return;
    n = cw_node_parent(n);
  }
}

void ui_mark_layout_dirty(cw_node n) {
  cw_control *c = CTL(n);
  if (!c) return;
  c->_flags |= CF_LAYOUT_DIRTY;
  for (int guard = 0; guard < 256; guard++) {
    if (c->_flags & CF_TOP_LEVEL) return;
    n = cw_node_parent(n);
    c = CTL(n);
    if (!c) return;
    c->_flags |= CF_SUBTREE_DIRTY;
  }
}

static bool mark_all_fn(cw_node n, void *u) {
  (void)u;
  cw_control *c = CTL(n);
  if (c) c->_flags |= CF_MIN_DIRTY | CF_LAYOUT_DIRTY | CF_SUBTREE_DIRTY;
  return true;
}
static void mark_subtree(cw_node n) {
  cw_node_walk(n, mark_all_fn, NULL);
  ui_mark_min_dirty(n);
}

void cw_control_update_min_size(cw_node n) { ui_mark_min_dirty(n); }
void cw_control_queue_layout(cw_node n) { ui_mark_layout_dirty(n); }
void cw_control_queue_redraw(cw_node n) { (void)n; }

/* ── measure ────────────────────────────────────────────────────────── */

cw_vec2 cw_control_min_size(cw_node n) {
  cw_control *c = CTL(n);
  if (!c) return (cw_vec2){ 0, 0 };
  if (c->_flags & CF_MIN_DIRTY) {
    c->_flags &= ~CF_MIN_DIRTY;
    cw_vec2 content = { 0, 0 };
    if (c->_vt->min_size) content = c->_vt->min_size(n, cw_node_data_t(n, c->_vt->min_size_t));
    c->_content_min = content;
    c->min_size.x = CW_MAX(content.x, c->custom_minimum_size.x);
    c->min_size.y = CW_MAX(content.y, c->custom_minimum_size.y);
    UI.stats.measured++;
  }
  return c->min_size;
}

/* ── arrange ────────────────────────────────────────────────────────── */

static CW_ARRAY(cw_node) resized_queue;

/* wall clock for the layout timing stat (reported, never fed to state) */
static double wall_s(void) {
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (double)ts.tv_sec + (double)ts.tv_nsec * 1e-9;
}

static void grow(float *lo, float *hi, float min, int dir) {
  if (*hi - *lo >= min) return;
  if (dir == CW_GROW_BEGIN) *lo = *hi - min;
  else if (dir == CW_GROW_BOTH) {
    float mid = (*lo + *hi) * 0.5f;
    *lo = ui_round(mid - min * 0.5f);
    *hi = *lo + min;
  } else *hi = *lo + min;
}

static cw_rect anchor_rect(cw_node n, cw_vec2 ps) {
  cw_control *c = CTL(n);
  cw_vec2 m = cw_control_min_size(n);
  float l = ui_round(c->anchor_left * ps.x + c->offset_left), r = ui_round(c->anchor_right * ps.x + c->offset_right);
  float t = ui_round(c->anchor_top * ps.y + c->offset_top), b = ui_round(c->anchor_bottom * ps.y + c->offset_bottom);
  grow(&l, &r, m.x, c->grow_horizontal);
  grow(&t, &b, m.y, c->grow_vertical);
  return (cw_rect){ l, t, r - l, b - t };
}

/* right to left: children placed left to right are mirrored inside the
 * parent's width (containers with their own scrolling or a split grabber
 * keep left-to-right placement) */
static bool mirrors(cw_node n, cw_control *c) {
  if (c->_vt->child_offset) return false;
  if (cw_node_is(n, "HSplitContainer")) return false;
  return cw_control_is_rtl(n);
}
static void mirror_children(cw_node n, float w) {
  int k = cw_node_child_count(n);
  for (int i = 0; i < k; i++) {
    cw_control *cc = CTL(cw_node_child(n, i));
    if (!cc || (cc->_flags & CF_TOP_LEVEL) || !cc->visible) continue;
    cc->rect.x = w - cc->rect.x - cc->rect.w;
  }
}

void ui_arrange(cw_node n, cw_rect r) {
  cw_control *c = CTL(n);
  if (!c) return;
  cw_vec2 m = cw_control_min_size(n);
  if (r.w < m.x) r.w = m.x;
  if (r.h < m.y) r.h = m.y;
  bool size_changed = r.w != c->rect.w || r.h != c->rect.h;
  c->rect.x = r.x;
  c->rect.y = r.y;
  if (!size_changed && !(c->_flags & (CF_LAYOUT_DIRTY | CF_SUBTREE_DIRTY | CF_NEVER_LAID))) return;
  if (size_changed && !(c->_flags & CF_NEVER_LAID)) cw_array_push(&resized_queue, n);
  c->rect = r;
  bool full = size_changed || (c->_flags & (CF_LAYOUT_DIRTY | CF_NEVER_LAID));
  c->_flags &= ~(CF_LAYOUT_DIRTY | CF_SUBTREE_DIRTY | CF_NEVER_LAID);
  UI.stats.arranged++;
  if (c->_vt->layout) {
    if (full) {
      c->_vt->layout(n, cw_node_data_t(n, c->_vt->layout_t), (cw_vec2){ r.w, r.h });
      if (mirrors(n, c)) mirror_children(n, r.w);
      return;
    }
    /* only descendants are dirty: re-arrange those children in place */
    int k = cw_node_child_count(n);
    for (int i = 0; i < k; i++) {
      cw_node ch = cw_node_child(n, i);
      cw_control *cc = CTL(ch);
      if (cc && !(cc->_flags & CF_TOP_LEVEL) && cc->visible &&
          (cc->_flags & (CF_LAYOUT_DIRTY | CF_SUBTREE_DIRTY | CF_NEVER_LAID)))
        ui_arrange(ch, cc->rect);
    }
    return;
  }
  int k = cw_node_child_count(n);
  for (int i = 0; i < k; i++) {
    cw_node ch = cw_node_child(n, i);
    cw_control *cc = CTL(ch);
    if (!cc || (cc->_flags & CF_TOP_LEVEL) || !cc->visible) continue;
    ui_arrange(ch, anchor_rect(ch, (cw_vec2){ r.w, r.h }));
  }
  if (mirrors(n, c)) mirror_children(n, r.w);
}

void cw_control_fit_child(cw_node child, cw_rect r) {
  cw_control *c = CTL(child);
  if (!c) return;
  cw_vec2 m = cw_control_min_size(child);
  cw_rect o = r;
  int hf = c->size_flags_horizontal, vf = c->size_flags_vertical;
  if (!(hf & CW_SIZE_FILL)) {
    o.w = CW_MIN(m.x, r.w);
    if (hf & CW_SIZE_SHRINK_CENTER) o.x = r.x + ui_round((r.w - o.w) * 0.5f);
    else if (hf & CW_SIZE_SHRINK_END) o.x = r.x + r.w - o.w;
  }
  if (!(vf & CW_SIZE_FILL)) {
    o.h = CW_MIN(m.y, r.h);
    if (vf & CW_SIZE_SHRINK_CENTER) o.y = r.y + ui_round((r.h - o.h) * 0.5f);
    else if (vf & CW_SIZE_SHRINK_END) o.y = r.y + r.h - o.h;
  }
  ui_arrange(child, o);
}

static bool collect_roots(cw_node n, void *u) {
  ui_tree_state *s = u;
  cw_control *c = CTL(n);
  if (c) {
    if (!(c->_flags & CF_TOP_LEVEL)) cw_array_push(&s->roots, n);
    return false; /* a control's children are its own */
  }
  return true;
}

static void ui_overlay_pass(cw_tree *t, int w, int h, double alpha, void *user) {
  (void)w, (void)h, (void)alpha, (void)user;
  cw_ui_render(t);
}

void ui_roots_refresh(ui_tree_state *s) {
  if (!s->roots_dirty) return;
  s->roots_dirty = false;
  cw_array_clear(&s->roots);
  cw_node_walk(cw_tree_root(s->tree), collect_roots, s);
  if (s->roots.count && !s->pass_registered) {
    /* the scene runner's canvas draws the UI over 3D and 2D (scene2d.h) */
    s->pass_registered = true;
    cw_canvas_add_pass(s->tree, 100, ui_overlay_pass, NULL);
  }
}

cw_vec2 cw_ui_canvas_size(void) {
  int w, h;
  float sc = ui_canvas_scale();
  if (UI.draw_virtual) {
    cw_vec2 cs = cw_r2d_canvas_size();
    return (cw_vec2){ cs.x / sc, cs.y / sc };
  }
  cw_window_size(&w, &h); /* logical: what the window pass's canvas addresses */
  return (cw_vec2){ floorf((float)w / sc), floorf((float)h / sc) };
}

static bool dirty_any(cw_control *c) { return (c->_flags & (CF_LAYOUT_DIRTY | CF_SUBTREE_DIRTY | CF_NEVER_LAID | CF_MIN_DIRTY)) != 0; }

void ui_layout_tree(ui_tree_state *s) {
  ui_roots_refresh(s);
  cw_vec2 canvas = cw_ui_canvas_size();
  double t0 = wall_s();
  bool did = false;
  UI.stats.measured = UI.stats.arranged = 0;
  for (int iter = 0; iter < 4; iter++) {
    bool any = false;
    for (int i = 0; i < s->roots.count; i++) {
      cw_node n = s->roots.data[i];
      cw_control *c = CTL(n);
      if (!c || !c->visible) continue;
      cw_rect want = anchor_rect(n, canvas);
      if (cw_control_is_rtl(n)) want.x = canvas.x - want.x - want.w;
      if (!dirty_any(c) && want.x == c->rect.x && want.y == c->rect.y && want.w == c->rect.w && want.h == c->rect.h) continue;
      any = true;
      ui_arrange(n, want);
    }
    for (int i = 0; i < s->popups.count; i++) {
      cw_node n = s->popups.data[i].node;
      cw_control *c = CTL(n);
      if (!c || !dirty_any(c)) continue;
      any = true;
      ui_arrange(n, c->rect);
    }
    if (!any) break;
    did = true;
  }
  if (did) {
    UI.stats.layout_passes++;
    UI.stats.layout_ms = (float)((wall_s() - t0) * 1000.0);
  }
  /* resized fires after the whole pass, when every rect is final */
  for (int i = 0; i < resized_queue.count; i++)
    if (cw_node_valid(resized_queue.data[i])) ui_emit0(resized_queue.data[i], "resized");
  cw_array_clear(&resized_queue);
}

/* ── setters ────────────────────────────────────────────────────────── */

void cw_control_set_anchors(cw_node n, float l, float t, float r, float b) {
  cw_control *c = CTL(n);
  if (!c) return;
  c->anchor_left = l, c->anchor_top = t, c->anchor_right = r, c->anchor_bottom = b;
  ui_mark_min_dirty(n);
}
void cw_control_set_offsets(cw_node n, float l, float t, float r, float b) {
  cw_control *c = CTL(n);
  if (!c) return;
  c->offset_left = l, c->offset_top = t, c->offset_right = r, c->offset_bottom = b;
  ui_mark_min_dirty(n);
}
void cw_control_set_rect(cw_node n, cw_rect r) {
  cw_control_set_anchors(n, 0, 0, 0, 0);
  cw_control_set_offsets(n, r.x, r.y, r.x + r.w, r.y + r.h);
}
void cw_control_set_anchors_preset(cw_node n, cw_anchor_preset p, float m) {
  cw_control *c = CTL(n);
  if (!c) return;
  static const float A[16][4] = {
    { 0, 0, 0, 0 }, { 1, 0, 1, 0 }, { 0, 1, 0, 1 }, { 1, 1, 1, 1 },
    { 0, .5f, 0, .5f }, { .5f, 0, .5f, 0 }, { 1, .5f, 1, .5f }, { .5f, 1, .5f, 1 }, { .5f, .5f, .5f, .5f },
    { 0, 0, 0, 1 }, { 0, 0, 1, 0 }, { 1, 0, 1, 1 }, { 0, 1, 1, 1 },
    { 0, .5f, 1, .5f }, { .5f, 0, .5f, 1 }, { 0, 0, 1, 1 } };
  if ((int)p < 0 || (int)p > 15) return;
  const float *a = A[p];
  float w = c->rect.w > 0 ? c->rect.w : cw_control_min_size(n).x;
  float h = c->rect.h > 0 ? c->rect.h : cw_control_min_size(n).y;
  /* per axis: a stretched axis gets margins; a point axis keeps the size */
  float ol, or_, ot, ob;
  if (a[0] != a[2]) ol = m, or_ = -m;
  else if (a[0] == 0) ol = m, or_ = m + w;
  else if (a[0] == 1) ol = -m - w, or_ = -m;
  else ol = -ui_round(w * 0.5f), or_ = ol + w;
  if (a[1] != a[3]) ot = m, ob = -m;
  else if (a[1] == 0) ot = m, ob = m + h;
  else if (a[1] == 1) ot = -m - h, ob = -m;
  else ot = -ui_round(h * 0.5f), ob = ot + h;
  c->anchor_left = a[0], c->anchor_top = a[1], c->anchor_right = a[2], c->anchor_bottom = a[3];
  c->offset_left = ol, c->offset_top = ot, c->offset_right = or_, c->offset_bottom = ob;
  if (a[0] == .5f && a[2] == .5f) c->grow_horizontal = CW_GROW_BOTH;
  if (a[1] == .5f && a[3] == .5f) c->grow_vertical = CW_GROW_BOTH;
  if (a[0] == 1 && a[2] == 1) c->grow_horizontal = CW_GROW_BEGIN;
  if (a[1] == 1 && a[3] == 1) c->grow_vertical = CW_GROW_BEGIN;
  ui_mark_min_dirty(n);
}
void cw_control_set_size_flags(cw_node n, int h, int v) {
  cw_control *c = CTL(n);
  if (!c) return;
  c->size_flags_horizontal = h, c->size_flags_vertical = v;
  ui_mark_min_dirty(n);
}
void cw_control_set_stretch_ratio(cw_node n, float r) {
  cw_control *c = CTL(n);
  if (!c) return;
  c->size_flags_stretch_ratio = r;
  ui_mark_min_dirty(n);
}
void cw_control_set_custom_min_size(cw_node n, cw_vec2 s) {
  cw_control *c = CTL(n);
  if (!c) return;
  c->custom_minimum_size = s;
  ui_mark_min_dirty(n);
}
void cw_control_set_mouse_filter(cw_node n, cw_mouse_filter f) {
  cw_control *c = CTL(n);
  if (c) c->mouse_filter = f;
}
void cw_control_set_focus_mode(cw_node n, cw_focus_mode m) {
  cw_control *c = CTL(n);
  if (c) c->focus_mode = m;
}
void cw_control_set_tooltip(cw_node n, const char *t) {
  cw_control *c = CTL(n);
  if (c) c->tooltip_text = t ? cw_intern(t) : NULL;
}
static void visibility_changed(cw_node n) {
  cw_control *c = CTL(n);
  if (!c->visible) ui_forget_node(n);
  ui_mark_min_dirty(n);
  c->_flags |= CF_NEVER_LAID;
}
void cw_control_set_visible(cw_node n, bool v) {
  cw_control *c = CTL(n);
  if (!c || c->visible == v) return;
  c->visible = v;
  visibility_changed(n);
}
void cw_control_set_modulate(cw_node n, cw_color col) {
  cw_control *c = CTL(n);
  if (c) c->modulate = col;
}
void cw_control_set_clip(cw_node n, bool clip) {
  cw_control *c = CTL(n);
  if (c) c->clip_contents = clip;
}
bool cw_control_visible_in_tree(cw_node n) {
  for (int guard = 0; guard < 256; guard++) {
    cw_control *c = CTL(n);
    if (!c) return true;
    if (!c->visible) return false;
    if (c->_flags & CF_TOP_LEVEL) return true;
    n = cw_node_parent(n);
  }
  return true;
}
bool cw_control_is_rtl(cw_node n) {
  for (int guard = 0; guard < 256; guard++) {
    cw_control *c = CTL(n);
    if (!c) break;
    if (c->layout_direction == CW_LAYOUT_LTR) return false;
    if (c->layout_direction == CW_LAYOUT_RTL) return true;
    if (c->layout_direction == CW_LAYOUT_LOCALE) return cw_l10n_is_rtl();
    if (c->_flags & CF_TOP_LEVEL) break;
    n = cw_node_parent(n);
  }
  return cw_l10n_is_rtl();
}
void cw_control_set_layout_direction(cw_node n, cw_layout_direction d) {
  cw_control *c = CTL(n);
  if (!c || c->layout_direction == (int32_t)d) return;
  c->layout_direction = (int32_t)d;
  mark_subtree(n);
}
void cw_control_set_auto_translate(cw_node n, bool on) {
  cw_control *c = CTL(n);
  if (!c || c->auto_translate == on) return;
  c->auto_translate = on;
  mark_subtree(n);
}
const char *cw_control_tr(cw_node n, const char *s) {
  cw_control *c = CTL(n);
  if (!s) return "";
  return c && c->auto_translate ? cw_tr_auto(s) : s;
}

/* the locale changed: every control re-measures (text, direction) */
static void locale_changed(void *u) {
  (void)u;
  cw_tree *t = cw_current_tree();
  if (!t) return;
  ui_tree_state *s = ui_state(t);
  ui_roots_refresh(s);
  for (int i = 0; i < s->roots.count; i++) mark_subtree(s->roots.data[i]);
  for (int i = 0; i < s->popups.count; i++) mark_subtree(s->popups.data[i].node);
}
void ui_l10n_listen(void) { cw_l10n_on_change(locale_changed, NULL); }

void cw_control_set_focus_neighbor(cw_node n, int side, cw_node nb) {
  cw_control *c = CTL(n);
  if (!c) return;
  cw_node *f[4] = { &c->focus_neighbor_left, &c->focus_neighbor_top, &c->focus_neighbor_right, &c->focus_neighbor_bottom };
  if (side >= 0 && side < 4) *f[side] = nb;
}

cw_rect cw_control_rect(cw_node n) {
  cw_control *c = CTL(n);
  return c ? c->rect : (cw_rect){ 0 };
}
cw_rect cw_control_global_rect(cw_node n) {
  cw_control *c = CTL(n);
  if (!c) return (cw_rect){ 0 };
  cw_rect r = c->rect;
  for (int guard = 0; guard < 256; guard++) {
    if (c->_flags & CF_TOP_LEVEL) break;
    cw_node p = cw_node_parent(n);
    cw_control *pc = CTL(p);
    if (!pc) break;
    cw_vec2 off = ui_child_offset(p);
    r.x += pc->rect.x + off.x;
    r.y += pc->rect.y + off.y;
    n = p, c = pc;
  }
  return r;
}

/* ── the node type ──────────────────────────────────────────────────── */

static void control_init(cw_node n, void *d) {
  cw_control *c = d;
  ui_init_once();
  c->size_flags_horizontal = c->size_flags_vertical = CW_SIZE_FILL;
  c->size_flags_stretch_ratio = 1;
  c->visible = true;
  c->modulate = c->self_modulate = CW_WHITE;
  c->mouse_filter = CW_MOUSE_STOP;
  c->auto_translate = true;
  c->_flags = CF_MIN_DIRTY | CF_LAYOUT_DIRTY | CF_NEVER_LAID;
  c->_vt = ui_vt_for(cw_node_get_type(n));
  UI.stats.controls++;
}

static void control_destroy(cw_node n, void *d) {
  cw_control *c = d;
  ui_forget_node(n);
  if (c->_override_id) cw_theme_destroy((cw_theme){ c->_override_id });
  UI.stats.controls--;
}

static void control_enter(cw_node n, void *d) {
  cw_control *c = d;
  c->_flags |= CF_IN_TREE | CF_NEVER_LAID;
  UI.theme_epoch++;
  cw_node p = cw_node_parent(n);
  if (CTL(p) && !(c->_flags & CF_TOP_LEVEL)) ui_mark_min_dirty(p);
  else {
    ui_tree_state *s = ui_state(cw_node_tree(n));
    if (s) s->roots_dirty = true;
  }
  c->_flags |= CF_MIN_DIRTY | CF_LAYOUT_DIRTY;
}

static void control_exit(cw_node n, void *d) {
  cw_control *c = d;
  ui_forget_node(n);
  c->_flags &= ~CF_IN_TREE;
  cw_node p = cw_node_parent(n);
  if (CTL(p) && !(c->_flags & CF_TOP_LEVEL)) ui_mark_min_dirty(p);
  else {
    ui_tree_state *s = ui_state(cw_node_tree(n));
    if (s) s->roots_dirty = true;
  }
}

static void control_children_changed(cw_node n, void *d) {
  (void)d;
  ui_mark_min_dirty(n);
}

static void apply_overrides_text(cw_node n, cw_control *c) {
  if (c->_override_id) cw_theme_destroy((cw_theme){ c->_override_id });
  c->_override_id = 0;
  if (!c->theme_overrides || !*c->theme_overrides) return;
  cw_theme t = cw_theme_new((cw_theme){ 0 });
  c->_override_id = t.id;
  const char *p = c->theme_overrides;
  while (*p) {
    const char *e = strchr(p, ';');
    /* a ';' inside style(...) does not end the item */
    const char *paren = strchr(p, '(');
    if (paren && e && paren < e) {
      const char *close = strchr(paren, ')');
      if (close) e = strchr(close, ';');
    }
    int len = e ? (int)(e - p) : (int)strlen(p);
    char line[512];
    snprintf(line, sizeof line, "%.*s", len, p);
    char *s = line;
    while (*s == ' ' || *s == '\t' || *s == '\n') s++;
    if (*s) {
      /* items without a type apply to any type: prefix "." */
      char full[600];
      snprintf(full, sizeof full, ".%s", s);
      if (!cw_theme_set_text(t, full)) cw_warn("'%s'.theme_overrides: cannot read '%s'", cw_node_name(n), s);
    }
    p = e ? e + 1 : p + len;
  }
}

static void control_prop_changed(cw_node n, void *d, const cw_prop *p) {
  cw_control *c = d;
  const char *nm = p->name;
  if (!strcmp(nm, "visible")) {
    visibility_changed(n);
  } else if (!strcmp(nm, "theme")) {
    c->_theme_id = c->theme && *c->theme ? cw_theme_load(c->theme).id : 0;
    UI.theme_epoch++;
    mark_subtree(n);
  } else if (!strcmp(nm, "theme_overrides")) {
    apply_overrides_text(n, c);
    UI.theme_epoch++;
    mark_subtree(n);
  } else if (!strcmp(nm, "layout_direction") || !strcmp(nm, "auto_translate")) {
    mark_subtree(n);
  } else if (!strncmp(nm, "accessibility_", 14)) {
    /* no layout effect */
  } else if (!strcmp(nm, "tooltip_text") || !strcmp(nm, "modulate") || !strcmp(nm, "self_modulate") ||
             !strcmp(nm, "mouse_filter") || !strcmp(nm, "focus_mode") || !strcmp(nm, "clip_contents") ||
             !strncmp(nm, "focus_", 6)) {
    /* no layout effect */
  } else {
    ui_mark_min_dirty(n);
  }
}

static void get_size(cw_node n, void *d, cw_value *o) {
  (void)n;
  cw_control *c = d;
  *o = cw_val_vec2(c->rect.w, c->rect.h);
}
static void get_position(cw_node n, void *d, cw_value *o) {
  (void)n;
  cw_control *c = d;
  *o = cw_val_vec2(c->rect.x, c->rect.y);
}
static void get_global_position(cw_node n, void *d, cw_value *o) {
  (void)d;
  cw_rect r = cw_control_global_rect(n);
  *o = cw_val_vec2(r.x, r.y);
}
static void get_min(cw_node n, void *d, cw_value *o) {
  (void)d;
  cw_vec2 m = cw_control_min_size(n);
  *o = cw_val_vec2(m.x, m.y);
}

static const char *const grow_names[] = { "END", "BEGIN", "BOTH", NULL };
static const char *const filter_names[] = { "STOP", "PASS", "IGNORE", NULL };
static const char *const focus_names[] = { "NONE", "CLICK", "ALL", NULL };
static const char *const dir_names[] = { "INHERITED", "LOCALE", "LTR", "RTL", NULL };

#define F(field, ty, d_) CW_PROP_FIELD(cw_control, field, ty, CW_PROP_DEFAULT, d_)
#define E(field, names, d_) \
  { .name = #field, .type = CW_T_ENUM, .flags = CW_PROP_DEFAULT, .offset = (int)offsetof(cw_control, field), \
    .size = (int)sizeof(((cw_control *)0)->field), .enum_names = names, .doc = d_ }
static const cw_prop control_props[] = {
  F(anchor_left, CW_T_FLOAT, "Left edge anchor, 0..1 of the parent's width."),
  F(anchor_top, CW_T_FLOAT, "Top edge anchor, 0..1 of the parent's height."),
  F(anchor_right, CW_T_FLOAT, "Right edge anchor."),
  F(anchor_bottom, CW_T_FLOAT, "Bottom edge anchor."),
  F(offset_left, CW_T_FLOAT, "Left edge offset from its anchor, UI units."),
  F(offset_top, CW_T_FLOAT, "Top edge offset."),
  F(offset_right, CW_T_FLOAT, "Right edge offset."),
  F(offset_bottom, CW_T_FLOAT, "Bottom edge offset."),
  E(grow_horizontal, grow_names, "Which way the rect grows when smaller than the minimum size."),
  E(grow_vertical, grow_names, "Which way the rect grows vertically."),
  F(size_flags_horizontal, CW_T_INT, "Inside a container: 1 FILL, 2 EXPAND, 4 SHRINK_CENTER, 8 SHRINK_END."),
  F(size_flags_vertical, CW_T_INT, "Vertical size flags."),
  F(size_flags_stretch_ratio, CW_T_FLOAT, "Share of the expand space relative to expanding siblings."),
  F(custom_minimum_size, CW_T_VEC2, "Minimum size on top of the content's."),
  E(mouse_filter, filter_names, "STOP handles and stops pointer events, PASS bubbles them, IGNORE is click-through."),
  E(focus_mode, focus_names, "NONE, CLICK (pointer only) or ALL (pointer, Tab, gamepad)."),
  F(tooltip_text, CW_T_STRING, "Shown after the pointer rests on the control."),
  F(visible, CW_T_BOOL, "Hidden controls hide their children and take no space in containers."),
  F(clip_contents, CW_T_BOOL, "Clip drawing and hit tests of children to the rect."),
  F(modulate, CW_T_COLOR, "Colour multiplier for this control and its children."),
  F(self_modulate, CW_T_COLOR, "Colour multiplier for this control only."),
  F(theme, CW_T_STRING, "Theme file (asset path) for this subtree."),
  F(theme_overrides, CW_T_STRING, "Per-control theme items: 'font_color = #ff0000; font_size = 20'."),
  F(focus_neighbor_left, CW_T_NODE, "Explicit focus target for left."),
  F(focus_neighbor_top, CW_T_NODE, "Explicit focus target for up."),
  F(focus_neighbor_right, CW_T_NODE, "Explicit focus target for right."),
  F(focus_neighbor_bottom, CW_T_NODE, "Explicit focus target for down."),
  F(focus_next, CW_T_NODE, "Explicit Tab target."),
  F(focus_previous, CW_T_NODE, "Explicit Shift+Tab target."),
  F(auto_translate, CW_T_BOOL, "Shown text goes through the translation catalogs (text that is a key shows its translation)."),
  E(layout_direction, dir_names, "INHERITED (the parent's; the locale's at the top), LOCALE, LTR or RTL: RTL mirrors children."),
  F(accessibility_name, CW_T_STRING, "What assistive technology announces ('' = the control's text). Translated like text."),
  F(accessibility_description, CW_T_STRING, "Longer help for assistive technology ('' = the tooltip)."),
  CW_PROP_ACCESSOR("size", CW_T_VEC2, CW_PROP_EDITOR | CW_PROP_READONLY, get_size, NULL, "Computed size."),
  CW_PROP_ACCESSOR("position", CW_T_VEC2, CW_PROP_EDITOR | CW_PROP_READONLY, get_position, NULL, "Computed position in the parent."),
  CW_PROP_ACCESSOR("global_position", CW_T_VEC2, CW_PROP_EDITOR | CW_PROP_READONLY, get_global_position, NULL, "Computed position on the canvas."),
  CW_PROP_ACCESSOR("minimum_size", CW_T_VEC2, CW_PROP_EDITOR | CW_PROP_READONLY, get_min, NULL, "Combined minimum size."),
  CW_PROP_END
};
#undef F
#undef E

static const char *const control_signals[] = { "focus_entered", "focus_exited", "mouse_entered", "mouse_exited",
                                               "resized", NULL };

static cw_node_type control_type = {
  .name = "Control",
  .parent = "Node",
  .data_size = sizeof(cw_control),
  .props = control_props,
  .signals = control_signals,
  .doc = "Base UI node: a rect from anchors + offsets (or from its container), theme, focus, input.",
  .init = control_init,
  .destroy = control_destroy,
  .enter_tree = control_enter,
  .exit_tree = control_exit,
  .prop_changed = control_prop_changed,
  .children_changed = control_children_changed,
};

void ui_register_control(void) {
  cw_node_type_register(&control_type);
  ui_control_t = &control_type;
  cw_ui_class_register("Control", &(cw_ui_class){ .theme_type = "Control" });
}

/* ── registration of everything ─────────────────────────────────────── */

void cw_ui_register_types(void) {
  static bool done;
  if (done) return;
  done = true;
  ui_register_control();
  ui_register_containers();
  ui_register_scroll();
  ui_register_labels();
  ui_register_buttons();
  ui_register_ranges();
  ui_register_line_edit();
  ui_register_text_edit();
  ui_register_lists();
  ui_register_tree();
  ui_register_popups();
  ui_register_misc();
  ui_register_captions();
  ui_register_remap();
}

/* UI module internals shared by the files in src/ui. */
#ifndef CW_UI_INTERNAL_H
#define CW_UI_INTERNAL_H

#include <ctype.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cartwheel/core.h"
#include "cartwheel/input.h"
#include "cartwheel/ui.h"

/* ── control flags (cw_control._flags) ─────────────────────────────── */
enum {
  CF_MIN_DIRTY = 1u << 0,     /* content min size must be recomputed */
  CF_LAYOUT_DIRTY = 1u << 1,  /* children must be re-arranged */
  CF_SUBTREE_DIRTY = 1u << 2, /* some descendant has LAYOUT/MIN dirty */
  CF_TOP_LEVEL = 1u << 3,     /* popup: rect is global, drawn in the popup layer */
  CF_NEVER_LAID = 1u << 4,
  CF_FOCUSED = 1u << 5,
  CF_HOVERED = 1u << 6,
  CF_IN_TREE = 1u << 7,
  CF_INTERNAL = 1u << 8,      /* engine-owned orphan (a widget's popup) */
};

/* ── classes resolved per node type ─────────────────────────────────── */
#define UI_MAX_THEME_TYPES 8
typedef struct cw__ui_vt {
  const cw_node_type *type;
  const char *theme_types[UI_MAX_THEME_TYPES]; /* interned, most derived first */
  int ntheme_types;
  cw_vec2 (*min_size)(cw_node, void *);
  const cw_node_type *min_size_t;
  void (*layout)(cw_node, void *, cw_vec2);
  const cw_node_type *layout_t;
  void (*draw)(cw_node, void *);
  const cw_node_type *draw_t;
  void (*draw_over)(cw_node, void *);
  const cw_node_type *draw_over_t;
  bool (*input)(cw_node, void *, const cw_ui_event *);
  const cw_node_type *input_t;
  bool (*has_point)(cw_node, void *, cw_vec2);
  const cw_node_type *has_point_t;
  void (*focus_changed)(cw_node, void *, bool);
  const cw_node_type *focus_changed_t;
  cw_vec2 (*child_offset)(cw_node, void *);
  const cw_node_type *child_offset_t;
  bool (*children_point)(cw_node, void *, cw_vec2);
  const cw_node_type *children_point_t;
  bool wants_text;
  bool is_container; /* has a layout method (children ignore anchors) */
} cw__ui_vt;

const cw__ui_vt *ui_vt_for(const cw_node_type *t);
extern const cw_node_type *ui_control_t; /* "Control" */

static inline cw_control *CTL(cw_node n) { return (cw_control *)cw_node_data_t(n, ui_control_t); }

/* ── per-tree state ─────────────────────────────────────────────────── */
typedef struct ui_popup {
  cw_node node;
  bool modal;          /* blocks input below; outside clicks do nothing */
  bool close_outside;  /* a press outside closes it (menus) */
  cw_node owner;       /* who opened it (submenu chain / option button) */
  cw_node prev_focus;  /* focus when it opened: restored when it closes (keyboard/pad users keep their place) */
} ui_popup;

typedef struct ui_tree_state {
  cw_tree *tree;
  CW_ARRAY(cw_node) roots;
  bool roots_dirty;
  CW_ARRAY(ui_popup) popups;
  cw_node focus, hover, capture, pressed_target;
  cw_node scroll_steal; /* ScrollContainer that took a touch drag */
  bool focus_visible;   /* last focus move came from keys/pad: draw the focus style */
  bool pass_registered; /* the scene runner draws us as a canvas overlay pass */
} ui_tree_state;

ui_tree_state *ui_state(cw_tree *t); /* created on demand */
ui_tree_state *ui_state_of(cw_node n); /* NULL when n is not in a tree (internal popups: their owner's) */

/* ── system ─────────────────────────────────────────────────────────── */
typedef struct ui_system {
  bool inited;
  float scale;         /* user scale (0 = 1) */
  float dpi;           /* drawable / logical */
  int layer;
  cw_theme project_theme;
  uint32_t theme_epoch;
  cw_ui_stats stats;
  /* pointer */
  cw_vec2 pointer;
  bool pointer_valid;
  uint32_t buttons, prev_buttons;
  bool from_touch;
  cw_vec2 press_pos;
  double last_click_time;
  cw_vec2 last_click_pos;
  int click_count;
  bool wants_pointer, wants_keyboard;
  double time;
  /* key repeat */
  uint8_t prev_keys[CW_KEY_COUNT]; /* last frame's held keys (scripted input sets no press bits) */
  int repeat_key;
  double repeat_at;
  int repeat_nav;
  double nav_repeat_at;
  uint32_t nav_held;
  cw_action nav_actions[6];
  /* text from events, consumed by the next process */
  char text_queue[512];
  int text_len;
  /* tooltip */
  cw_node tip_node;
  double tip_timer;
  bool tip_shown;
  cw_vec2 tip_pos;
  cw_ui_unhandled_fn unhandled;
  void *unhandled_user;
  bool draw_virtual; /* pointer mapped through the caller's pass canvas */
  bool text_input_on;
  int32_t dbg_focus_hash;
  /* drag and drop (dnd.c) */
  bool drag_active;
  bool drag_cancel_release; /* a drag was canceled while the left button was down: that release is canceled too */
  cw_ui_drag drag;          /* kind interned; text and label point at the buffers below */
  char drag_text[1024], drag_label[256];
  cw_node drop_target;      /* accepting target under the pointer, null = none */
  cw_rect drop_highlight;   /* UI units, canvas origin */
  uint32_t drops;           /* completed drops (stats/tests) */
  /* accessibility */
  float text_scale;         /* multiplies every theme font size (0 = 1) */
  bool reduced_motion;      /* no caret blink, no tooltip delay, no smooth motion */
  cw_node remap_capture;    /* RemapButton listening for a new binding */
} ui_system;
extern ui_system UI;

void ui_init_once(void);
float ui_canvas_scale(void); /* UI units -> canvas pixels */

/* accessibility (a11y.c) */
void ui_a11y_init(void);
void ui_a11y_focus_changed(cw_node n);
void ui_a11y_value_changed(cw_node n, const char *signal);
void ui_remap_frame(void); /* remap.c: poll an active binding capture */
void ui_captions_frame(double dt); /* captions.c */

/* localization (control.c, draw.c) */
void ui_l10n_listen(void);                         /* re-measure on locale changes */
static inline const char *ui_tr(cw_node n, const char *s) { return cw_control_tr(n, s); }
bool ui_draw_rtl(void);                            /* the control being drawn lays out right to left */

/* layout (control.c) */
void ui_mark_min_dirty(cw_node n);
void ui_mark_layout_dirty(cw_node n);
void ui_arrange(cw_node n, cw_rect r);
void ui_layout_tree(ui_tree_state *s);
cw_vec2 ui_child_offset(cw_node n);
cw_node ui_parent_control(cw_node n);
bool ui_is_top_level(cw_node n);
void ui_roots_refresh(ui_tree_state *s);
void ui_emit0(cw_node n, const char *signal);
void ui_emit_int(cw_node n, const char *signal, int64_t v);
void ui_emit_bool(cw_node n, const char *signal, bool v);
void ui_emit_float(cw_node n, const char *signal, double v);
void ui_emit_str(cw_node n, const char *signal, const char *s);

/* theme (theme.c) */
void ui_theme_init(void);
const char *ui_theme_type(cw_node n); /* most derived theme type */
bool ui_lookup(cw_node n, const char *type, const char *name, int kind, void *out);
enum { TI_COLOR, TI_CONST, TI_FONT, TI_STYLE };
cw_font ui_font_load_cached(const char *path);
/* common shortcuts for widget code (type NULL = the control's own) */
static inline cw_color T_COLOR(cw_node n, const char *name) { return cw_control_theme_color(n, NULL, name); }
static inline float T_CONST(cw_node n, const char *name) { return cw_control_theme_constant(n, NULL, name); }
static inline cw_ui_style T_STYLE(cw_node n, const char *name) { return cw_control_theme_style(n, NULL, name); }
static inline cw_font T_FONT(cw_node n) { return cw_control_theme_font(n, NULL, "font"); }
static inline float T_FSIZE(cw_node n) { return cw_control_theme_font_size(n, NULL, "font_size"); }
/* content margins of a style */
static inline float SM_W(const cw_ui_style *s) { return s->margin[0] + s->margin[2]; }
static inline float SM_H(const cw_ui_style *s) { return s->margin[1] + s->margin[3]; }
static inline cw_rect style_content(const cw_ui_style *s, cw_rect r) {
  return (cw_rect){ r.x + s->margin[0], r.y + s->margin[1], r.w - SM_W(s), r.h - SM_H(s) };
}

/* drag and drop (dnd.c) */
void ui_dnd_update(cw_node under, cw_vec2 pointer); /* each frame while a drag is active */
void ui_dnd_release(void);                          /* the button came up: drop or cancel */
void ui_dnd_draw(ui_tree_state *s);

/* drawing (draw.c) */
void ui_draw_tree(ui_tree_state *s);
void ui_draw_begin_control(cw_node n, cw_vec2 origin);
cw_vec2 ui_draw_origin(void);
void ui_draw_set_origin(cw_vec2 o);
void ui_draw_focus_style(cw_node n);
void ui_draw_text_styled(cw_font f, const char *s, cw_vec2 global, cw_text_style *st);

/* input (input.c) */
void ui_input_frame(ui_tree_state *s, double dt);
bool ui_focusable(cw_node n, bool by_keyboard);
void ui_popup_open(cw_node n, bool modal, bool close_outside, cw_node owner);
void ui_popup_close(cw_node n);
bool ui_popup_is_open(cw_node n);
void ui_popups_close_all(ui_tree_state *s);
void ui_set_capture(cw_node n);
bool ui_shift(void);
bool ui_ctrl(void);
void ui_text_input_update(ui_tree_state *s);
void ui_forget_node(cw_node n); /* a control leaves the tree / is freed */

/* scrollbar helper (scroll.c): drawn and driven by its owner */
typedef struct ui_scrollbar {
  bool vertical;
  float value, page, max; /* value 0..max-page */
  bool dragging, hovered;
  float drag_from_value, drag_from_pos;
  cw_rect rect;           /* owner-local, set by the owner each layout */
} ui_scrollbar;
float ui_scrollbar_thickness(cw_node owner);
void ui_scrollbar_draw(cw_node owner, ui_scrollbar *sb);
/* true when the event was the scrollbar's; *changed when value moved */
bool ui_scrollbar_input(cw_node owner, ui_scrollbar *sb, const cw_ui_event *e, bool *changed);
static inline bool ui_scrollbar_needed(const ui_scrollbar *sb) { return sb->max > sb->page + 0.5f; }
static inline float ui_scrollbar_clamp(const ui_scrollbar *sb, float v) {
  float hi = sb->max - sb->page;
  if (v > hi) v = hi;
  if (v < 0) v = 0;
  return v;
}

/* menu logic shared by PopupMenu, OptionButton's list and MenuBar (popup.c) */
typedef struct ui_menu_item {
  char *label;
  int id;
  bool checkable, checked, separator, disabled;
  const char *submenu; /* interned child PopupMenu name */
  const char *shortcut; /* interned accelerator text drawn at the right ("Ctrl+Z"); NULL = none */
} ui_menu_item;

/* utf8 helpers (text.c) */
int ui_utf8_next(const char *s, int len, int i); /* byte after the codepoint at i */
int ui_utf8_prev(const char *s, int i);
int ui_utf8_count(const char *s, int len);
uint32_t ui_utf8_decode(const char *s, int len, int *i);
bool ui_is_word_char(uint32_t c);

/* text field helpers (line_edit.c) */
int ui_byte_at_x(cw_font f, float size, const char *s, int len, float x);
int ui_word_left(const char *s, int i);
int ui_word_right(const char *s, int len, int i);

/* dynamic strings */
typedef struct ui_str { char *s; int len, cap; } ui_str;
void ui_str_set(ui_str *b, const char *s, int len);
void ui_str_insert(ui_str *b, int at, const char *s, int len);
void ui_str_erase(ui_str *b, int at, int n);
void ui_str_free(ui_str *b);
static inline const char *ui_str_c(const ui_str *b) { return b->s ? b->s : ""; }

/* visible, non-popup control children (frame arena) */
int ui_children(cw_node n, cw_node **out);
bool ui_scroll_steal(cw_node scroll, cw_vec2 from, cw_vec2 now);

/* registration helpers */
void ui_register_control(void);
void ui_register_containers(void);
void ui_register_scroll(void);
void ui_register_labels(void);
void ui_register_buttons(void);
void ui_register_ranges(void);
void ui_register_line_edit(void);
void ui_register_text_edit(void);
void ui_register_lists(void);
void ui_register_tree(void);
void ui_register_popups(void);
void ui_register_misc(void);
void ui_register_captions(void);
void ui_register_remap(void);

/* text props stored as interned strings: a getter/setter pair keeps an owned
 * copy elsewhere where edits are frequent (LineEdit, TextEdit). */
static inline const char *ui_s(const char *s) { return s ? s : ""; }
static inline bool rect_has(cw_rect r, cw_vec2 p) { return p.x >= r.x && p.y >= r.y && p.x < r.x + r.w && p.y < r.y + r.h; }
static inline float ui_round(float v) { return floorf(v + 0.5f); }

#endif

/* cartwheel UI: a retained widget system built from scene NODES, for games
 * and for the editor (which is built with it).
 *
 * Every widget is a node type extending Control (Control extends Node), so
 * controls live in the scene tree, load from .cwscene files, have reflected
 * properties (cw_node_set / cw_node_get_prop), emit signals (cw_connect) and
 * are freed with their subtree. They draw through the 2D renderer
 * (render2d.h) on their own canvas layer above the game.
 *
 * ── Rects ────────────────────────────────────────────────────────────
 * Units are UI units: canvas pixels divided by the UI scale (cw_ui_set_scale;
 * 1 by default, 2 for a pixel-art game's chunky UI, the display's density
 * for HiDPI). A control's rect is relative to its parent control (or to the
 * canvas for a top-level control, one whose parent is not a Control).
 * The canvas is the WINDOW, not a CanvasSettings virtual canvas: Node2Ds
 * use virtual pixels, controls do not (scene2d.h, "two coordinate
 * spaces"), unless the UI is drawn into a virtual pass with cw_ui_draw.
 *
 *   - Under a plain Control (or the canvas) a child's rect comes from its
 *     ANCHORS (0..1 of the parent's size, per side) plus OFFSETS (UI units,
 *     per side), Godot-style: left = anchor_left * parent_w + offset_left,
 *     right = anchor_right * parent_w + offset_right. If that is smaller than
 *     the minimum size the rect grows (grow_horizontal / grow_vertical say
 *     which way).
 *   - Under a Container the container places children; their anchors and
 *     offsets are ignored. Size flags say what a child does with the space
 *     it gets: FILL (take the cross size), EXPAND (share the extra main-axis
 *     space by stretch ratio), SHRINK_CENTER / SHRINK_END (sit at min size,
 *     centred or at the end; neither = at the start).
 *
 * ── Layout ───────────────────────────────────────────────────────────
 * Two passes. MEASURE: every control's minimum size is max(custom minimum
 * size, content minimum size), where content comes from text and theme
 * (a Button: its text + its style's content margins; a VBox: its children
 * plus separations). Sizes always derive from content and theme; nothing is
 * a hard-coded pixel constant. ARRANGE: top-down, each container fits its
 * children. Both are cached: changing a property that affects size marks the
 * control and its ancestors dirty, and only dirty subtrees are measured and
 * re-arranged (a subtree whose size did not change and is clean is moved,
 * not re-laid out). Container positions and sizes are rounded to whole UI
 * units, so layouts are exact on every host. Layout runs once per frame
 * before input routing and again before drawing if something changed;
 * cw_ui_layout forces it.
 *
 * ── Localization ───────────────────────────────────────────────────────
 * Text a control shows (Label/RichTextLabel/Button text, tooltips,
 * OptionButton/ItemList/PopupMenu items, MenuBar and TabContainer titles,
 * LineEdit placeholders, dialog texts) goes through cw_tr_auto when the
 * control's auto_translate is on (the default): text that is a catalog key
 * shows its translation, other text shows as written. Changing the locale
 * re-measures every control. Right to left (layout_direction, resolved
 * through the ancestors to the locale): containers and anchors mirror their
 * children horizontally (HBox order, Grid columns, Margin sides, anchored
 * rects), LEFT/RIGHT text alignments swap, check boxes and option arrows
 * move to the other side, and text is shaped as an RTL paragraph. Scroll
 * containers, split containers, tab bars, trees, sliders, progress bars and
 * text fields keep their left-to-right internals.
 *
 * ── Accessibility ──────────────────────────────────────────────────────
 * Every focusable control is reachable by Tab / Shift+Tab, the arrow keys
 * and the gamepad (ui_* actions); closing a popup gives focus back to the
 * control that had it when it opened (an OptionButton after a choice), so
 * keyboard and gamepad users keep their place. cw_ui_set_text_scale,
 * cw_ui_set_reduced_motion and the high-contrast / colorblind themes are
 * below; the accessibility tree, captions and remapping are in
 * cartwheel/a11y.h. Controls carry accessibility_name /
 * accessibility_description for what text alone does not say.
 *
 * ── Input routing ─────────────────────────────────────────────────────
 * Once per frame (the platform calls the UI right after the input module,
 * BEFORE any fixed tick), the UI reads the raw input snapshot and routes:
 *   - pointer (mouse; a touch acts as the mouse): open popups first, then
 *     the topmost control under the pointer in reverse draw order whose
 *     mouse_filter is not IGNORE. Unhandled events bubble to the parent
 *     while the filter is PASS; STOP ends there. A press captures the
 *     pointer: moves and the release go to the pressed control until every
 *     button is up (drags). Inside a ScrollContainer a touch that moves
 *     past a few units becomes a scroll drag and the pressed control gets a
 *     canceled release.
 *   - keys and text: to the focus owner; unhandled Tab / Shift+Tab move
 *     focus in tree order, arrows / Enter / Space / Escape act as
 *     navigation.
 *   - gamepad: actions ui_up, ui_down, ui_left, ui_right, ui_accept and
 *     ui_cancel (cartwheel/input.h). The UI creates them with pad bindings
 *     (d-pad, left stick, south, east) unless the game defined them; the
 *     keyboard reaches navigation through the key path, so do not bind keys
 *     to ui_* actions. Navigation goes to the explicit focus neighbour, or
 *     else the nearest focusable control in that direction (geometry).
 *   - whatever the UI did not take is the game's: cw_ui_wants_pointer() /
 *     cw_ui_wants_keyboard() say whether to ignore the pointer/keys this
 *     frame, and an unhandled-input callback sees what fell through.
 * Text arrives as CW_EVENT_TEXT; the UI starts platform text input while a
 * text field has focus (cw_text_input_start).
 *
 * ── Signals (cw_connect(node, "pressed", ...)) ───────────────────────
 * Control: focus_entered, focus_exited, mouse_entered, mouse_exited, resized.
 * Buttons: pressed, toggled(bool). Range widgets: value_changed(float).
 * LineEdit: text_changed(string), text_submitted(string). TextEdit:
 * text_changed. ItemList / OptionButton / Tree: item_selected(int),
 * item_activated(int). Tree: item_collapsed(int, bool), multi_selected(int,
 * bool). PopupMenu / MenuBar:
 * id_pressed(int). TabContainer: tab_changed(int). RichTextLabel:
 * meta_clicked(string). Dialogs: confirmed, canceled. ColorPicker:
 * color_changed(color). Split: dragged(float). ScrollContainer:
 * scroll_changed.
 *
 * ── Drawing and cost ──────────────────────────────────────────────────
 * cw_ui_render draws every visible control of a tree in tree order (popups,
 * then tooltips on top) in its own 2D pass on the window, no clear, on canvas
 * layer cw_ui_layer() (default CW_R2D_MAX_LAYERS - 1). Styles (rounded
 * rects, borders, shadows, all anti-aliased) are vertex-coloured triangles
 * and text is SDF, so they batch together: a typical screen is a handful of
 * draw calls; clip_contents (scroll areas, text fields) costs a batch break
 * each. Drawing is O(visible controls); layout is O(dirty controls).
 *
 * Threads: main thread only. Memory: CW_MEM_UI. */
#ifndef CW_UI_H
#define CW_UI_H

#include "platform.h"
#include "render2d.h"
#include "scene.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ── enums ──────────────────────────────────────────────────────────── */

enum { /* size flags (bitmask; SHRINK_BEGIN = 0) */
  CW_SIZE_SHRINK_BEGIN = 0,
  CW_SIZE_FILL = 1,
  CW_SIZE_EXPAND = 2,
  CW_SIZE_EXPAND_FILL = 3,
  CW_SIZE_SHRINK_CENTER = 4,
  CW_SIZE_SHRINK_END = 8,
};
typedef enum { CW_MOUSE_STOP, CW_MOUSE_PASS, CW_MOUSE_IGNORE } cw_mouse_filter;
typedef enum { CW_FOCUS_NONE, CW_FOCUS_CLICK, CW_FOCUS_ALL } cw_focus_mode;
typedef enum { CW_GROW_END, CW_GROW_BEGIN, CW_GROW_BOTH } cw_grow_dir;
/* INHERITED: the parent control's; at the top, LOCALE. LOCALE: right to
 * left when the active locale is (cartwheel/l10n.h). */
typedef enum { CW_LAYOUT_INHERITED, CW_LAYOUT_LOCALE, CW_LAYOUT_LTR, CW_LAYOUT_RTL } cw_layout_direction;
typedef enum { CW_HALIGN_LEFT, CW_HALIGN_CENTER, CW_HALIGN_RIGHT, CW_HALIGN_FILL } cw_halign;
typedef enum { CW_VALIGN_TOP, CW_VALIGN_CENTER, CW_VALIGN_BOTTOM, CW_VALIGN_FILL } cw_valign;
typedef enum {
  CW_PRESET_TOP_LEFT, CW_PRESET_TOP_RIGHT, CW_PRESET_BOTTOM_LEFT, CW_PRESET_BOTTOM_RIGHT,
  CW_PRESET_CENTER_LEFT, CW_PRESET_CENTER_TOP, CW_PRESET_CENTER_RIGHT, CW_PRESET_CENTER_BOTTOM, CW_PRESET_CENTER,
  CW_PRESET_LEFT_WIDE, CW_PRESET_TOP_WIDE, CW_PRESET_RIGHT_WIDE, CW_PRESET_BOTTOM_WIDE,
  CW_PRESET_VCENTER_WIDE, CW_PRESET_HCENTER_WIDE, CW_PRESET_FULL_RECT
} cw_anchor_preset;

/* ── Control ───────────────────────────────────────────────────────────
 * The node data of type "Control". Reflected fields are listed first; write
 * them through cw_node_set or the setters below (which mark layout dirty);
 * writing the struct directly needs cw_control_queue_layout afterwards. */
typedef struct cw_control {
  float anchor_left, anchor_top, anchor_right, anchor_bottom; /* 0..1 of the parent */
  float offset_left, offset_top, offset_right, offset_bottom; /* UI units */
  int32_t grow_horizontal, grow_vertical;                     /* cw_grow_dir */
  int32_t size_flags_horizontal, size_flags_vertical;         /* CW_SIZE_* (default FILL) */
  float size_flags_stretch_ratio;                             /* share of EXPAND space (default 1) */
  cw_vec2 custom_minimum_size;
  int32_t mouse_filter;   /* cw_mouse_filter (default STOP; containers and labels IGNORE/PASS) */
  int32_t focus_mode;     /* cw_focus_mode */
  const char *tooltip_text;
  bool visible;
  bool clip_contents;     /* children and drawing are clipped to the rect (also for hit tests) */
  cw_color modulate;      /* multiplies this control and its children */
  cw_color self_modulate; /* this control only */
  const char *theme;      /* asset path of a theme file applying to this subtree ("" = inherit) */
  /* "font_color = #ff8040; font_size = 20; normal = style(bg=#202020)": theme
   * items for this control only (any item name; see the theme format) */
  const char *theme_overrides;
  cw_node focus_neighbor_left, focus_neighbor_top, focus_neighbor_right, focus_neighbor_bottom;
  cw_node focus_next, focus_previous;
  bool auto_translate;    /* shown text goes through cw_tr_auto (default true) */
  int32_t layout_direction; /* cw_layout_direction (default INHERITED) */
  const char *accessibility_name;        /* what a screen reader says ("" = from the text) */
  const char *accessibility_description; /* longer help ("" = the tooltip) */
  /* ── engine-owned: read only ── */
  cw_rect rect;           /* position relative to the parent control / canvas, size */
  cw_vec2 min_size;       /* last measured combined minimum size */
  /* private */
  uint32_t _flags;
  uint64_t _theme_id, _override_id, _eff_theme;
  cw_node _owner;         /* internal popups: the widget that owns them */
  uint32_t _theme_epoch;
  const struct cw__ui_vt *_vt;
  cw_vec2 _content_min;
} cw_control;

/* Data of a node's Control part (NULL when n is not a Control). */
cw_control *cw_control_of(cw_node n);
bool cw_is_control(cw_node n); /* n is a live node whose type extends Control */

/* 0..1 of the parent per side; ignored under a Container. Re-measures. */
void cw_control_set_anchors(cw_node n, float left, float top, float right, float bottom);
/* UI units per side, added to the anchored edges; ignored under a Container */
void cw_control_set_offsets(cw_node n, float left, float top, float right, float bottom);
/* Anchors for a preset; offsets keep the current size at that spot (margin
 * from the anchored edges), FULL_RECT and the *_WIDE presets stretch with
 * offsets = margin. */
void cw_control_set_anchors_preset(cw_node n, cw_anchor_preset preset, float margin);
/* Top-left anchored rect (anchors 0, offsets = rect): the common
 * "place it here, this big" for top-level controls. */
void cw_control_set_rect(cw_node n, cw_rect r);
void cw_control_set_size_flags(cw_node n, int h, int v); /* CW_SIZE_* for each axis (used by containers); re-measures */
void cw_control_set_stretch_ratio(cw_node n, float ratio); /* share of EXPAND space vs siblings (default 1) */
/* UI units; the min size is the max of this and the content */
void cw_control_set_custom_min_size(cw_node n, cw_vec2 size);
void cw_control_set_mouse_filter(cw_node n, cw_mouse_filter f); /* STOP / PASS / IGNORE for pointer hit tests */
void cw_control_set_focus_mode(cw_node n, cw_focus_mode m); /* NONE / CLICK / ALL: how the user can focus it */
/* shown after the hover delay; copied (interned); NULL = none */
void cw_control_set_tooltip(cw_node n, const char *text);
/* hidden: not drawn or hit-tested, loses hover/focus; re-measures */
void cw_control_set_visible(cw_node n, bool visible);
void cw_control_set_modulate(cw_node n, cw_color c); /* tint multiplied into this control and its children */
void cw_control_set_clip(cw_node n, bool clip); /* clip children, drawing and hit tests to the rect (one batch break) */
bool cw_control_visible_in_tree(cw_node n); /* it and every ancestor control visible */
/* LTR / RTL / LOCALE / INHERITED; re-measures the subtree */
void cw_control_set_layout_direction(cw_node n, cw_layout_direction d);
bool cw_control_is_rtl(cw_node n); /* the resolved direction lays this control out right to left */
void cw_control_set_auto_translate(cw_node n, bool on); /* shown text through cw_tr_auto; re-measures the subtree */
/* The text a control shows for s: translated when auto_translate is on. */
const char *cw_control_tr(cw_node n, const char *s);

/* Computed rects (lay out first: cw_ui_layout, or read after a frame). */
cw_rect cw_control_rect(cw_node n);        /* relative to the parent */
cw_rect cw_control_global_rect(cw_node n); /* UI units, canvas origin (scroll offsets included) */
cw_vec2 cw_control_min_size(cw_node n);    /* combined minimum size (measures if dirty) */

/* Mark size-affecting state changed (custom controls call this when their
 * content changes): re-measure this control and its ancestors. */
void cw_control_update_min_size(cw_node n);
/* Re-arrange this control's children at the next layout. */
void cw_control_queue_layout(cw_node n);
/* Request a redraw (drawing is immediate-mode every frame, so this only
 * matters to custom caches; provided for API symmetry). */
void cw_control_queue_redraw(cw_node n);

/* Focus. grab_focus works whatever the focus mode (code may focus anything
 * visible); the user reaches CLICK controls by pointer and ALL controls by
 * pointer, Tab and gamepad. */
void cw_control_grab_focus(cw_node n);
void cw_control_release_focus(cw_node n); /* drop focus if n has it (no-op otherwise) */
bool cw_control_has_focus(cw_node n); /* n is the focus owner of its tree */
/* Focus neighbours by node (explicit overrides geometry). */
void cw_control_set_focus_neighbor(cw_node n, int side /* 0 left 1 top 2 right 3 bottom */, cw_node neighbor);

/* Containers place children with this (custom containers too): applies the
 * child's size flags inside `r` (parent space) and arranges it. */
void cw_control_fit_child(cw_node child, cw_rect r);

/* ── theme ─────────────────────────────────────────────────────────────
 * A theme holds items by (theme type, name): styles ("Button" "hover"),
 * colors ("Label" "font_color"), constants ("BoxContainer" "separation";
 * font sizes are constants named *font_size) and fonts ("TextEdit" "font").
 * The empty type "" holds defaults for every type ("default_font",
 * "default_font_size"). A lookup tries the control's theme_overrides, then
 * the nearest ancestor-or-self control theme, then the project theme
 * (cw_ui_set_theme), then the built-in default; inside each theme the
 * control's type, then its base types (CheckBox falls back to Button), then
 * the theme's base theme. Missing everywhere: 0 / transparent / empty style.
 *
 * Text format (one item per line, '#' comments):
 *
 *   extends = dark                  # dark | light | none (default dark)
 *   default_font = fonts/Inter.ttf  # asset path (SDF), or builtin:sans / builtin:mono
 *   default_font_size = 15
 *   Button.font_color = #e8eaf0
 *   Button.normal = style(bg=#2c313c, border=1, border_color=#3d4452, radius=5, margin=12 6)
 *   Button.hover = style(base=Button.normal, bg=#353b48)
 *   BoxContainer.separation = 6
 *   Panel.panel = style(bg=#1e2128, shadow=#00000080, shadow_size=8, shadow_offset=0 2)
 *   LineEdit.focus = empty
 *
 * Colours are #rgb, #rrggbb or #rrggbbaa. A style's fields: bg, border
 * (1 or 4 numbers l t r b), border_color, radius (1 or 4: tl tr br bl),
 * margin (content margins: 1, 2 (h v) or 4 numbers), shadow, shadow_size,
 * shadow_offset, draw_center (0/1), base (start from another style). */
CW_HANDLE_TYPE(cw_theme);

typedef struct cw_ui_style {
  bool draw;                 /* false = an empty style (margins only) */
  bool draw_center;          /* fill the inside (default true) */
  cw_color bg;
  float border[4];           /* left top right bottom */
  cw_color border_color;
  float radius[4];           /* top-left top-right bottom-right bottom-left */
  float margin[4];           /* content margins: left top right bottom */
  cw_color shadow;
  float shadow_size;
  cw_vec2 shadow_offset;
} cw_ui_style;

/* A flat style (draw = true, draw_center = true). */
cw_ui_style cw_ui_style_flat(cw_color bg, float radius, float margin_h, float margin_v);

cw_theme cw_theme_new(cw_theme base); /* empty theme; base may be null */
/* The built-ins: dark (editor-like) and light, and two accessibility
 * variants of dark: high contrast (black/white/yellow, text over 15:1) and
 * colorblind-friendly (Okabe-Ito accents that stay apart under every common
 * colour vision deficiency). Owned by the module; "extends = high_contrast"
 * / "extends = colorblind" in theme text. */
cw_theme cw_theme_default_dark(void);
cw_theme cw_theme_default_light(void); /* built-in light theme; built on first call, owned by the module */
cw_theme cw_theme_high_contrast(void); /* built-in high-contrast dark variant; module-owned */
cw_theme cw_theme_colorblind(void); /* built-in colorblind-safe dark variant; module-owned */
/* Parse theme text (see above). NULL handle and a logged line number on an
 * error. name labels errors. Font paths load through cw_font_load. */
cw_theme cw_theme_parse(const char *text, const char *name);
/* Load + parse an asset; cached by path (the theme property uses this). */
cw_theme cw_theme_load(const char *asset_path);
/* Every item of this theme (not its base) as theme text; cw_alloc'd. */
char *cw_theme_save(cw_theme t);
/* free a theme made by new/parse; built-ins and null are ignored. Do not
 * destroy one in use. */
void cw_theme_destroy(cw_theme t);

/* add or replace; type NULL = "" (defaults for every type); null t ignored */
void cw_theme_set_color(cw_theme t, const char *type, const char *name, cw_color c);
/* add or replace (font sizes are constants named *font_size) */
void cw_theme_set_constant(cw_theme t, const char *type, const char *name, float v);
/* add or replace; the theme does not own the font */
void cw_theme_set_font(cw_theme t, const char *type, const char *name, cw_font f);
/* add or replace; *s copied */
void cw_theme_set_style(cw_theme t, const char *type, const char *name, const cw_ui_style *s);
/* One "Type.name = value" line (the text format) applied to t. */
bool cw_theme_set_text(cw_theme t, const char *line);
/* Direct lookups in one theme (and its bases); false when missing. */
bool cw_theme_get_color(cw_theme t, const char *type, const char *name, cw_color *out);
bool cw_theme_get_constant(cw_theme t, const char *type, const char *name, float *out); /* false when missing */
bool cw_theme_get_font(cw_theme t, const char *type, const char *name, cw_font *out); /* false when missing */
bool cw_theme_get_style(cw_theme t, const char *type, const char *name, cw_ui_style *out); /* false when missing */

/* The project theme (the fallback for every control; null = built-in dark). */
void cw_ui_set_theme(cw_theme t);
cw_theme cw_ui_theme(void); /* the project theme set above (null = none set: built-in dark applies) */
/* A control's own theme (applies to its subtree; null = inherit). */
void cw_control_set_theme(cw_node n, cw_theme t);
/* Per-control overrides (same lookup names the control's type uses). */
void cw_control_override_color(cw_node n, const char *name, cw_color c);
/* per-control constant override; re-measures */
void cw_control_override_constant(cw_node n, const char *name, float v);
/* per-control font override (not owned); re-measures */
void cw_control_override_font(cw_node n, const char *name, cw_font f);
/* per-control style override (*s copied); re-measures */
void cw_control_override_style(cw_node n, const char *name, const cw_ui_style *s);
/* drop every per-control override, those from theme_overrides text included */
void cw_control_clear_overrides(cw_node n);

/* Resolved lookups for a control, through the whole chain above; type NULL =
 * the control's own theme type chain. Used by widget drawing code. */
cw_color cw_control_theme_color(cw_node n, const char *type, const char *name);
float cw_control_theme_constant(cw_node n, const char *type, const char *name); /* 0 when missing everywhere */
cw_font cw_control_theme_font(cw_node n, const char *type, const char *name);      /* falls back to default_font */
float cw_control_theme_font_size(cw_node n, const char *type, const char *name);   /* falls back to default_font_size */
/* zeroed (an empty style, draw = false) when missing everywhere */
cw_ui_style cw_control_theme_style(cw_node n, const char *type, const char *name);

/* The embedded fonts (SDF): Fira Sans (UI text) and Fira Mono (code), OFL. */
cw_font cw_ui_font_sans(void);
cw_font cw_ui_font_mono(void); /* Fira Mono, SDF; loaded on first call, owned by the module */

/* ── custom controls ───────────────────────────────────────────────────
 * A node type extending Control (or any widget) gets UI behaviour from a
 * class: register the node type, then its class. Methods left NULL inherit
 * from the nearest base type's class. Each method gets the data of the
 * type the class was registered for. */
typedef enum {
  CW_UI_MOUSE_MOVE, CW_UI_MOUSE_DOWN, CW_UI_MOUSE_UP, CW_UI_WHEEL, CW_UI_KEY, CW_UI_TEXT, CW_UI_NAV
} cw_ui_event_type;
typedef enum { CW_NAV_UP, CW_NAV_DOWN, CW_NAV_LEFT, CW_NAV_RIGHT, CW_NAV_ACCEPT, CW_NAV_CANCEL } cw_ui_nav;

typedef struct cw_ui_event {
  cw_ui_event_type type;
  cw_vec2 pos;      /* pointer, local to the receiving control */
  cw_vec2 global;   /* pointer, UI units */
  int button;       /* MOUSE_DOWN/UP: 0 left 1 right 2 middle */
  uint32_t buttons; /* held */
  int clicks;       /* MOUSE_DOWN: 2 = double click */
  bool touch;       /* pointer from a touch */
  bool canceled;    /* MOUSE_UP: the press was taken over (scroll drag): do not act */
  int key;          /* KEY: SDL scancode */
  bool repeat;      /* KEY / NAV: auto-repeat of a held key or direction */
  bool shift, ctrl, alt;
  const char *text; /* TEXT: UTF-8 */
  cw_vec2 wheel;    /* WHEEL: notches, +y up */
  int nav;          /* NAV: cw_ui_nav (from a pad action, or an unhandled key) */
} cw_ui_event;

typedef struct cw_ui_class {
  const char *theme_type;  /* theme type name ("Button"); NULL = the node type's name */
  /* content minimum size (without custom_minimum_size) */
  cw_vec2 (*min_size)(cw_node n, void *data);
  /* place children inside `size` (cw_control_fit_child); NULL = by anchors */
  void (*layout)(cw_node n, void *data, cw_vec2 size);
  /* draw yourself with the cw_ui_draw_* helpers (local coordinates) */
  void (*draw)(cw_node n, void *data);
  /* drawn after the children (scrollbars, focus overlays) */
  void (*draw_over)(cw_node n, void *data);
  /* return true when handled (stops bubbling) */
  bool (*input)(cw_node n, void *data, const cw_ui_event *e);
  /* point (local) inside the clickable shape; NULL = the rect */
  bool (*has_point)(cw_node n, void *data, cw_vec2 local);
  /* focus gained / lost */
  void (*focus_changed)(cw_node n, void *data, bool focused);
  /* this control takes text input while focused (LineEdit, TextEdit, SpinBox) */
  bool wants_text;
  /* offset applied to children's positions (scrolling); NULL = none */
  cw_vec2 (*child_offset)(cw_node n, void *data);
  /* children are hit-testable at this local point (a scroll viewport);
   * NULL = anywhere (inside the rect when clip_contents) */
  bool (*children_point)(cw_node n, void *data, cw_vec2 local);
} cw_ui_class;
/* Attach (or replace) the class of a registered node type; *cls copied.
 * Derived types re-resolve. */
void cw_ui_class_register(const char *node_type, const cw_ui_class *cls);

/* Drawing helpers: valid inside draw callbacks, coordinates local to the
 * control being drawn, colours multiplied by the inherited modulate. */
void cw_ui_draw_style(const cw_ui_style *s, cw_rect r);
void cw_ui_draw_rect(cw_rect r, cw_color c); /* filled axis-aligned rect */
void cw_ui_draw_rect_lines(cw_rect r, float width, cw_color c); /* rect outline, width in UI units */
void cw_ui_draw_round_rect(cw_rect r, float radius, cw_color c); /* filled, anti-aliased rounded rect (a flat style) */
void cw_ui_draw_line(cw_vec2 a, cw_vec2 b, float width, cw_color c); /* width in UI units */
void cw_ui_draw_circle(cw_vec2 center, float radius, cw_color c); /* filled, anti-aliased */
void cw_ui_draw_polygon(const cw_vec2 *pts, int n, cw_color c); /* filled convex polygon; n >= 3 (else nothing) */
/* Per-vertex coloured triangles (gradients). */
void cw_ui_draw_triangles(const cw_vec2 *pos, const cw_color *col, int nv, const uint32_t *idx, int ni);
/* Texture region src scaled into dst; tint 0 (all zero) = white. */
void cw_ui_draw_texture(cw_texture t, cw_rect dst, cw_rect src /* w 0 = whole */, cw_color tint);
/* Nine-slice of src with margins l tp r b (texels); center false = hollow;
 * tint 0 = white. */
void cw_ui_draw_nine_patch(cw_texture t, cw_rect dst, cw_rect src, float l, float tp, float r, float b, bool center,
                           cw_color tint);
/* Text with its box's top-left at pos; returns its width. */
float cw_ui_draw_text(cw_font f, float size, const char *utf8, cw_vec2 pos, cw_color c);
/* Text vertically centred in r with horizontal alignment; clipped to r's
 * width with an ellipsis when it does not fit and `ellipsis` is set. */
void cw_ui_draw_text_in(cw_font f, float size, const char *utf8, cw_rect r, cw_halign align, cw_color c, bool ellipsis);
/* Simple glyph icons drawn with shapes: arrows, check mark, cross, dot. */
typedef enum { CW_ICON_ARROW_DOWN, CW_ICON_ARROW_RIGHT, CW_ICON_ARROW_UP, CW_ICON_ARROW_LEFT, CW_ICON_CHECK,
               CW_ICON_CROSS, CW_ICON_DOT } cw_ui_icon;
void cw_ui_draw_icon(cw_ui_icon icon, cw_rect r, cw_color c); /* centred in r, sized to its shorter side */
void cw_ui_push_clip(cw_rect r); /* intersect the clip with r (local); balance with pop; max depth 39 */
void cw_ui_pop_clip(void); /* undo the last push_clip (extra pops are ignored) */

/* Text metrics helpers (pure; usable in min_size). */
float cw_ui_text_width(cw_font f, float size, const char *utf8);
float cw_ui_line_height(cw_font f, float size); /* line advance for size (UI units) */

/* ── the UI system ─────────────────────────────────────────────────────── */

/* UI units -> canvas pixels (0 or 1 = UI units are logical window pixels,
 * times the display density). */
void cw_ui_set_scale(float scale);
float cw_ui_scale(void); /* current scale (1 when unset) */
void cw_ui_set_layer(int r2d_layer); /* 2D canvas layer the UI draws on; clamped to 0..CW_R2D_MAX_LAYERS-1 */
int cw_ui_layer(void); /* the UI layer (default CW_R2D_MAX_LAYERS - 1) */
/* The canvas size in UI units (the rect top-level anchors resolve against). */
cw_vec2 cw_ui_canvas_size(void);

/* Route this frame's input and lay out. The platform calls it every frame
 * for cw_current_tree() once any Control exists; call it yourself only for
 * another tree. */
void cw_ui_process(cw_tree *t, double dt);
/* Lay out every dirty control of the tree now (no input). */
void cw_ui_layout(cw_tree *t);
/* Draw the tree's UI in its own window pass (above everything drawn so far);
 * the scene runner calls it after the app's render. The zoom camera it puts
 * on cw_ui_layer() is taken back after that pass (the layer's settings are
 * as before), so a later pass on that layer is not scaled. */
void cw_ui_render(cw_tree *t);
/* Accessibility: a multiplier on every theme font size (clamped 0.5..4; 1 =
 * as themed); layouts grow with the text. Reduced motion: no caret blink,
 * tooltips without the hover delay, no animated UI motion; games read it
 * with cw_ui_reduced_motion for their own effects (screen shake, parallax,
 * camera bob, flashing). */
void cw_ui_set_text_scale(float scale);
float cw_ui_text_scale(void); /* current text scale (1 when unset) */
void cw_ui_set_reduced_motion(bool on); /* global flag, read live (no relayout) */
bool cw_ui_reduced_motion(void); /* the flag above (games read it for their own effects) */
/* Draw into the CALLER's current pass instead (e.g. a virtual-canvas
 * pixel-art pass); the pointer is then mapped through that pass's canvas. */
void cw_ui_draw(cw_tree *t);
/* Feed an app event (the platform does this automatically). */
void cw_ui_feed_event(const cw_event *e);

bool cw_ui_wants_pointer(void);  /* pointer over UI, or a UI drag in progress, this frame */
bool cw_ui_wants_keyboard(void); /* a control has keyboard focus */
cw_node cw_ui_focus_owner(void); /* focused control of the current tree; null = none */
cw_node cw_ui_hovered(void); /* control under the pointer in the current tree; null = none */
cw_vec2 cw_ui_pointer(void);     /* UI units */
/* Called with each pointer/key/nav event no control handled. */
typedef void (*cw_ui_unhandled_fn)(const cw_ui_event *e, void *user);
void cw_ui_set_unhandled_input(cw_ui_unhandled_fn fn, void *user); /* one global callback; NULL clears */

typedef struct cw_ui_stats {
  uint32_t controls;      /* Control nodes alive */
  uint32_t drawn;         /* controls drawn last frame */
  uint32_t layout_passes; /* layout runs that found dirty work (all time) */
  uint32_t measured;      /* min sizes recomputed, last layout */
  uint32_t arranged;      /* controls (re)arranged, last layout */
  uint32_t draw_calls;    /* GL draws of the last cw_ui_render pass */
  uint32_t commands;      /* 2D commands the UI submitted last frame */
  uint32_t popups;        /* open popups */
  float layout_ms;        /* last layout wall time (reported, never compared) */
} cw_ui_stats;
const cw_ui_stats *cw_ui_get_stats(void); /* module-owned, updated each frame; popups counted for the current tree */
/* Register the stats as debug fields "<prefix>.<name>". */
void cw_ui_debug_register(const char *prefix);

/* Register every UI node type (cw_scene_register_builtin_types calls it). */
void cw_ui_register_types(void);

/* ── Containers (node types) ───────────────────────────────────────────
 * Container: base type, fits children to its rect.
 * HBoxContainer / VBoxContainer: a row / column; theme constant
 *   "separation"; property `alignment` (0 begin 1 center 2 end) for the
 *   leftover space when no child expands.
 * GridContainer: `columns`; constants "h_separation", "v_separation". A
 *   column (row) expands when any child in it has EXPAND.
 * MarginContainer: constants "margin_left/top/right/bottom".
 * CenterContainer: children at their minimum size, centred.
 * Panel: draws style "panel"; children placed by anchors.
 * PanelContainer: style "panel", children fit inside its content margins.
 * ScrollContainer: one content child at its minimum size (stretched to fill
 *   when smaller), scrolled by wheel, scrollbar drag, touch drag, and
 *   keyboard focus moving inside (ensure-visible). Properties
 *   `horizontal_scroll_mode` / `vertical_scroll_mode` (0 disabled, 1 auto,
 *   2 always), `scroll_horizontal` / `scroll_vertical`.
 * HSplitContainer / VSplitContainer: two children with a draggable
 *   separator; `split_offset`, `collapsed`; constant "separation".
 * TabContainer: one page per child (tab title = child name, or its
 *   its line of the `tab_titles` property, one title per line, set
 *   directly or through cw_tab_container_set_title); `current_tab`;
 *   `drag_to_rearrange`: dragging a tab starts a UI drag of kind "tab"
 *   (see drag and drop below) that drop targets (another TabContainer, a
 *   dock) accept or refuse; the container itself moves nothing.
 * HFlowContainer / VFlowContainer: children wrap into lines.
 * AspectRatioContainer: `ratio`, `stretch_mode` (0 width controls height,
 *   1 height controls width, 2 fit, 3 cover), `alignment_horizontal`,
 *   `alignment_vertical` (0 begin 1 center 2 end). */
void cw_scroll_set(cw_node scroll, cw_vec2 offset);
cw_vec2 cw_scroll_get(cw_node scroll); /* current scroll offset (UI units); 0,0 when not a ScrollContainer */
cw_vec2 cw_scroll_max(cw_node scroll); /* largest offset */
/* scroll the least needed to show descendant (lay out first); emits
 * scroll_changed */
void cw_scroll_ensure_visible(cw_node scroll, cw_node descendant);
/* UI units from the natural split spot; clamped to the children's min sizes
 * at layout */
void cw_split_set_offset(cw_node split, float offset);
float cw_split_offset(cw_node split); /* the requested offset (0 when not a split container) */
/* clamped to the pages; emits tab_changed when it changes */
void cw_tab_container_set_current(cw_node tabs, int index);
int cw_tab_container_current(cw_node tabs); /* -1 when not a TabContainer */
/* Override page index's tab title (copied; "" or NULL = the page name; a
 * newline in it becomes a space; negative index ignored). Any number and
 * length of titles. */
void cw_tab_container_set_title(cw_node tabs, int index, const char *title);
/* Pages are the Control children that are not popups, in child order. */
int cw_tab_container_page_count(cw_node tabs);
cw_node cw_tab_container_page(cw_node tabs, int index); /* null when out of range */
const char *cw_tab_container_title(cw_node tabs, int index); /* frame-arena copy; NULL when out of range */
/* The tab under a point local to the container (-1 = none: not on the tab
 * bar), and a tab's rect in local coordinates (lay out first). */
int cw_tab_container_tab_at(cw_node tabs, cw_vec2 local);
cw_rect cw_tab_container_tab_rect(cw_node tabs, int index); /* zero rect when out of range */

/* ── Widgets (node types) ──────────────────────────────────────────────
 * Label: `text`, `horizontal_alignment` (cw_halign), `vertical_alignment`
 *   (cw_valign), `autowrap` (wrap at the width; min width 0), `clip_text`,
 *   `ellipsis` (cut with "..." to the width), `uppercase`. Theme:
 *   font, font_size, font_color.
 * RichTextLabel: `text` with BBCode [b] [i] [color=#rrggbb] [color=name]
 *   [size=N] [url=meta]...[/url]; `fit_content` (min height = text). Bold
 *   and italic use the theme fonts "bold_font" / "italics_font" when set,
 *   else synthetic (double-struck / sheared). Clicking a [url] emits
 *   meta_clicked(meta).
 * Button: `text`, `disabled`, `toggle_mode`, `button_pressed`,
 *   `button_group` (name: toggle buttons sharing a group in one tree are
 *   exclusive), `alignment`, `flat`. Styles normal / hover / pressed /
 *   disabled / focus; colors font_color, font_hover_color,
 *   font_pressed_color, font_disabled_color.
 * CheckBox / CheckButton: toggle buttons drawn as a box / a switch.
 * OptionButton: a button with a popup list; `items` ("A\nB\nC"),
 *   `selected`; emits item_selected.
 * HSlider / VSlider / SpinBox / ProgressBar extend Range: `min_value`,
 *   `max_value`, `step` (0 = continuous), `value`, `page` (the value
 *   stops at max_value - page, as in Godot: a scrollbar thumb of size page
 *   ends at max_value). SpinBox: arrows,
 *   typed numbers, `suffix`, `prefix`. ProgressBar: `show_percentage`.
 * LineEdit: `text`, `placeholder_text`, `max_length` (characters, 0 = no
 *   limit), `secret` (draws `secret_character`), `editable`; caret +
 *   selection (mouse drag, double-click word, shift+arrows/home/end, ctrl+a),
 *   clipboard (ctrl+c/x/v), undo (ctrl+z).
 * TextEdit: `text`, `editable`, `show_line_numbers`, `highlight_current_line`;
 *   long lines scroll horizontally (no wrapping); multi-line caret and selection, scrolling, clipboard, a
 *   syntax highlighter hook (cw_text_edit_set_highlighter; cw_ui_highlight_c
 *   is a ready one for C).
 * ItemList: items with optional icons, `select_mode` (0 single 1 multi),
 *   `items` text; scrolling.
 * Tree: rows of items with columns, expand/collapse, selection; `columns`,
 *   `hide_root`, `items` (indented text: two spaces per level, '|' between
 *   columns) for scene files; the C API below for code. `select_mode` 1 =
 *   multiple selection: ctrl+click toggles a row, shift+click selects the
 *   range from the last clicked row, a plain click selects one row (on
 *   release when it was already selected, so a selection can be dragged);
 *   every membership change emits multi_selected(item, selected) and the
 *   focused row still emits item_selected. `drag_items`: pressing a row
 *   and moving starts a UI drag of kind "tree_item" (payload = the item's
 *   metadata, text = its first column, label "N items" for a multi drag);
 *   make the tree (or anything) a drop target and place the drop with
 *   cw_uitree_item_at's section (above / on / below a row).
 * PopupMenu: items (label, id, check, separator, submenu = child PopupMenu
 *   name); `items` text: "Open|1\n-\nRecent>RecentMenu\n[x]Grid|7".
 *   cw_popup_menu_popup shows it at a point; emits id_pressed(id). An item
 *   may show shortcut text at its right (cw_popup_menu_set_shortcut; theme
 *   color font_shortcut_color); the menu only draws it, the app binds keys.
 * PopupPanel: a Popup that draws style "panel" and fits its children inside
 *   the panel's content margins (command palettes, quick pickers);
 *   cw_popup_show places and opens it (any Popup, in fact).
 * MenuBar: one title per child PopupMenu (its `title`, else its name).
 * AcceptDialog / ConfirmationDialog: `title`, `dialog_text`, `ok_text`,
 *   `cancel_text`; modal; cw_dialog_popup_centered; confirmed / canceled.
 * ColorPicker: `color`; HSV square + hue strip + alpha strip + hex readout.
 * TextureRect: `texture` (asset path) or cw_texture_rect_set_texture;
 *   `stretch_mode` (0 scale, 1 tile, 2 keep, 3 keep centered, 4 keep aspect,
 *   5 keep aspect centered, 6 keep aspect covered); `region_rect`; `filter`
 *   (LINEAR | NEAREST for pixel art). Path textures load once per path and
 *   filter, shared with the 2D nodes (Sprite2D ...).
 * NinePatchRect: `texture`, `patch_margin_left/top/right/bottom`,
 *   `draw_center`, `filter`.
 * CaptionPanel: the active captions and subtitles (cartwheel/a11y.h), one
 *   box per line, newest at the bottom; theme "CaptionPanel": panel,
 *   font, font_size, font_color, speaker_color, separation.
 * RemapButton: a Button showing an input action's binding (`action`,
 *   `slot`, `device` KEYBOARD_MOUSE | GAMEPAD); pressing it captures the
 *   next key/button/axis as the new binding; emits remapped(binding). */

void cw_label_set_text(cw_node n, const char *text);
const char *cw_label_text(cw_node n); /* interned, stable; "" when not a Label */

void cw_button_set_text(cw_node n, const char *text); /* copied (interned); NULL = ""; re-measures */
const char *cw_button_text(cw_node n); /* interned, stable; "" when not a Button */
void cw_button_set_pressed(cw_node n, bool pressed); /* toggle state; emits toggled when it changes */
bool cw_button_pressed(cw_node n); /* toggle state (false when not a Button) */
void cw_button_set_disabled(cw_node n, bool disabled); /* disabling drops a press in progress and the focus */
void cw_button_set_toggle_mode(cw_node n, bool toggle); /* toggle buttons keep their pressed state between clicks */
void cw_button_set_group(cw_node n, const char *group); /* exclusive group name (copied); NULL = none */

/* append (copied); selects item 0 if nothing was selected */
void cw_option_button_add_item(cw_node n, const char *label);
void cw_option_button_clear(cw_node n); /* remove every item; selection becomes -1 */
void cw_option_button_select(cw_node n, int index); /* clamped to -1..count-1; no item_selected (code, not the user) */
int cw_option_button_selected(cw_node n); /* selected index, -1 = none */
int cw_option_button_item_count(cw_node n); /* 0 when not an OptionButton */

void cw_range_set_value(cw_node n, double v); /* stepped, clamped to min..max-page; emits value_changed when it changes */
double cw_range_value(cw_node n); /* 0 when not a Range */
/* step 0 = continuous; re-clamps the value (may emit value_changed) */
void cw_range_set_limits(cw_node n, double min, double max, double step);

void cw_line_edit_set_text(cw_node n, const char *text); /* no text_changed (code, not the user) */
const char *cw_line_edit_text(cw_node n); /* owned by the field; valid until its next edit; "" when not a LineEdit */
void cw_line_edit_set_caret(cw_node n, int byte); /* byte offset, clamped to the text and moved back to a code point start; clears the selection */
int cw_line_edit_caret(cw_node n); /* byte offset of the caret */
void cw_line_edit_select(cw_node n, int from_byte, int to_byte); /* bytes, clamped and moved back to code point starts; caret ends at to_byte */
void cw_line_edit_selection(cw_node n, int *from_byte, int *to_byte); /* equal = none */

typedef struct cw_text_span { int start, end; cw_color color; } cw_text_span; /* byte range in the line */
/* Fill up to cap spans for one line (bytes not covered use font_color);
 * return the span count. Called for visible lines only, every draw. */
typedef int (*cw_highlight_fn)(cw_node text_edit, const char *line, int len, int line_index, cw_text_span *out, int cap,
                               void *user);
/* replace all (tabs become 4 spaces); no text_changed; scroll resets */
void cw_text_edit_set_text(cw_node n, const char *text);
/* The whole text, frame-arena copy. */
const char *cw_text_edit_text(cw_node n);
int cw_text_edit_line_count(cw_node n); /* >= 1 for a TextEdit; 0 when not one */
const char *cw_text_edit_line(cw_node n, int line); /* frame-arena copy */
/* clamped to the text; clears the selection, scrolls it into view */
void cw_text_edit_set_caret(cw_node n, int line, int column_byte);
/* either out pointer may be NULL; 0,0 when not a TextEdit */
void cw_text_edit_caret(cw_node n, int *line, int *column_byte);
void cw_text_edit_insert(cw_node n, const char *text); /* at the caret, replacing the selection */
/* fn NULL = plain font_color; user passed back to fn */
void cw_text_edit_set_highlighter(cw_node n, cw_highlight_fn fn, void *user);
/* A small C/C++ highlighter: keywords, types, strings, chars, numbers,
 * comments (line-local), preprocessor lines. Colors from the TextEdit theme:
 * keyword_color, type_color, string_color, number_color, comment_color,
 * preprocessor_color (looked up on text_edit). */
int cw_ui_highlight_c(cw_node text_edit, const char *line, int len, int line_index, cw_text_span *out, int cap, void *user);

/* text copied; icon not owned (null = none); returns the index, -1 when not
 * an ItemList */
int cw_item_list_add_item(cw_node n, const char *text, cw_texture icon);
void cw_item_list_clear(cw_node n); /* remove every item and reset scrolling */
int cw_item_list_count(cw_node n); /* 0 when not an ItemList */
/* owned by the list until the item is cleared; "" when out of range */
const char *cw_item_list_text(cw_node n, int index);
/* single (or select_mode 0) deselects the rest first; no signal */
void cw_item_list_select(cw_node n, int index, bool single);
void cw_item_list_deselect_all(cw_node n); /* no signal */
bool cw_item_list_is_selected(cw_node n, int index); /* false when out of range */
int cw_item_list_selected(cw_node n); /* first selected, -1 = none */
void cw_item_list_ensure_visible(cw_node n, int index); /* scroll the least needed to show the row (lay out first) */

/* Tree items are ints, stable until removed; -1 = none (the invisible root's
 * parent). Item 0 is the root once created. */
int cw_uitree_create_item(cw_node tree, int parent, const char *text);
void cw_uitree_clear(cw_node tree); /* remove every item (the root too); item ids restart */
void cw_uitree_remove_item(cw_node tree, int item); /* and its children */
void cw_uitree_set_text(cw_node tree, int item, int column, const char *text); /* copied; column 0..7 */
/* owned by the tree until changed or removed; "" when missing */
const char *cw_uitree_text(cw_node tree, int item, int column);
void cw_uitree_set_collapsed(cw_node tree, int item, bool collapsed); /* emits item_collapsed when it changes */
bool cw_uitree_collapsed(cw_node tree, int item); /* false when missing */
void cw_uitree_set_columns(cw_node tree, int count); /* clamped to 1..8; re-measures */
void cw_uitree_set_column_title(cw_node tree, int column, const char *title); /* copied (interned); NULL = none */
/* expand: take spare width; min_width in UI units */
void cw_uitree_set_column_expand(cw_node tree, int column, bool expand, float min_width);
void cw_uitree_set_metadata(cw_node tree, int item, int64_t meta); /* app number for the item (a node id, an index) */
int64_t cw_uitree_metadata(cw_node tree, int item); /* 0 when missing */
/* focus/select item, expand its ancestors, scroll to it; emits
 * item_selected on change */
void cw_uitree_select(cw_node tree, int item);
int cw_uitree_selected(cw_node tree); /* focused/selected item, -1 = none */
int cw_uitree_item_parent(cw_node tree, int item); /* -1 for the root or a missing item */
int cw_uitree_item_child_count(cw_node tree, int item); /* 0 when missing */
int cw_uitree_item_child(cw_node tree, int item, int index); /* -1 when out of range */
int cw_uitree_visible_rows(cw_node tree); /* rows currently shown (expanded items) */
/* select_mode 1: membership without signals (code syncing the tree to a
 * model); is_selected also answers single mode (the focused row). */
void cw_uitree_set_selected(cw_node tree, int item, bool selected);
bool cw_uitree_is_selected(cw_node tree, int item); /* false when missing */
void cw_uitree_deselect_all(cw_node tree); /* no signals */
/* Draw one column of an item in this colour instead of the theme's
 * font colours (dimmed inherited rows, warnings); NULL restores. */
void cw_uitree_set_custom_color(cw_node tree, int item, int column, const cw_color *color);
/* The item under a point local to the tree (-1 = none) and where in its row
 * (section -1 = top quarter, 0 = middle, 1 = bottom quarter: drop above,
 * onto, below). */
int cw_uitree_item_at(cw_node tree, cw_vec2 local, int *section);
/* Where item's row is on the canvas (UI units, like cw_control_global_rect):
 * zero size when it is not shown (a collapsed ancestor, scrolled out of
 * view). For tests and tools that click rows. */
cw_rect cw_uitree_item_rect(cw_node tree, int item);

int cw_popup_menu_add_item(cw_node menu, const char *label, int id);       /* id < 0 = the item index */
/* like add_item with a check mark (initially unchecked); returns the index */
int cw_popup_menu_add_check_item(cw_node menu, const char *label, int id);
void cw_popup_menu_add_separator(cw_node menu); /* a divider line (not selectable) */
/* opens the child PopupMenu named child_menu_name (copied); returns the
 * index */
int cw_popup_menu_add_submenu(cw_node menu, const char *label, const char *child_menu_name);
/* also makes the item checkable; ignored out of range */
void cw_popup_menu_set_checked(cw_node menu, int index, bool checked);
bool cw_popup_menu_checked(cw_node menu, int index); /* false when out of range */
/* greyed and not selectable; ignored out of range */
void cw_popup_menu_set_disabled(cw_node menu, int index, bool disabled);
void cw_popup_menu_set_shortcut(cw_node menu, int index, const char *text); /* NULL or "" clears */
const char *cw_popup_menu_item_text(cw_node menu, int index); /* NULL when out of range */
int cw_popup_menu_item_id(cw_node menu, int index);           /* -1 when out of range */
int cw_popup_menu_item_count(cw_node menu); /* separators included */
void cw_popup_menu_clear(cw_node menu); /* remove every item */
/* Show at a point (UI units); it closes on a choice, Escape or a click
 * outside. */
void cw_popup_menu_popup(cw_node menu, cw_vec2 at);
void cw_popup_hide(cw_node popup); /* close it and the popups it owns; focus returns to where it was */
bool cw_popup_visible(cw_node popup); /* open in its tree right now */
/* Open any Popup (a PopupPanel usually) at rect (UI units; w/h below its
 * minimum size grow to it; kept inside the canvas). modal: blocks input
 * below and stays open on outside clicks; otherwise an outside click or
 * Escape closes it. */
void cw_popup_show(cw_node popup, cw_rect rect, bool modal);

void cw_dialog_popup_centered(cw_node dialog); /* open modally at its min size, centred on the canvas; focus on OK */

void cw_color_picker_set_color(cw_node n, cw_color c); /* no color_changed (code, not the user) */
cw_color cw_color_picker_color(cw_node n); /* transparent black when not a ColorPicker */

/* ── drag and drop ─────────────────────────────────────────────────────
 * A control starts a drag from its input callback while the left button is
 * held (usually once the pointer moved a few units from the press):
 * cw_ui_drag_begin. Until the button comes up the UI draws `label` beside
 * the pointer (theme type "DragPreview": panel, font, font_size,
 * font_color), sends pointer moves to no control, and every frame asks the
 * drop targets under the pointer, innermost first, whether they accept; the
 * first that does is highlighted (theme "DropTarget" style "highlight").
 * Releasing over it calls its drop; releasing elsewhere or Escape cancels.
 * The source then gets its MOUSE_UP with `canceled` set either way, so it
 * never treats the end of a drag as a click. Built-in source: TabContainer
 * with drag_to_rearrange (kind "tab"). Drop targets are registered per node
 * (any Control), so an app makes any control a target without a subclass. */
typedef struct cw_ui_drag {
  cw_node source;    /* the control the drag started from */
  const char *kind;  /* what is dragged: "tab", "node", "file", ... (interned by begin) */
  int64_t payload;   /* kind-specific number (a tab index, a node id) */
  const char *text;  /* kind-specific text (a title, a path); copied by begin, up to 1023 bytes */
  const char *label; /* shown beside the pointer; copied, up to 255 bytes; "" = no preview */
} cw_ui_drag;
typedef struct cw_ui_drop_fns {
  /* May d drop at `local` (target coordinates)? Optionally narrow
   * *highlight (local; preset to the whole control) to show where it lands. */
  bool (*can_drop)(cw_node target, const cw_ui_drag *d, cw_vec2 local, cw_rect *highlight, void *user);
  /* The drop happened at local. */
  void (*drop)(cw_node target, const cw_ui_drag *d, cw_vec2 local, void *user);
  void *user;
} cw_ui_drop_fns;
/* Make n a drop target (fns copied); NULL removes. Entries of freed nodes
 * are dropped automatically. */
void cw_ui_set_drop_target(cw_node n, const cw_ui_drop_fns *fns);
/* Start a drag; false when the left button is not held or a drag is
 * already running. */
bool cw_ui_drag_begin(const cw_ui_drag *d);
void cw_ui_drag_cancel(void); /* end the current drag without a drop (no-op when none) */
const cw_ui_drag *cw_ui_drag_current(void); /* NULL when no drag is running */
cw_node cw_ui_drop_target(void);            /* the accepting target under the pointer now; null = none */

/* not owned; replaces the `texture` path; null = none; re-measures */
void cw_texture_rect_set_texture(cw_node n, cw_texture t);
void cw_nine_patch_set_texture(cw_node n, cw_texture t); /* not owned; replaces the `texture` path; null = none */

#ifdef __cplusplus
}
#endif
#endif

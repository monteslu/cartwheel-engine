/* cartwheel accessibility: the accessibility tree of the UI (roles, names,
 * values, states) for screen readers, live announcements, captions and
 * subtitles for audio, input remapping support.
 *
 * Settings that live with the UI module (ui.h): cw_ui_set_text_scale (text
 * size), cw_ui_set_reduced_motion, the high-contrast and colorblind themes
 * (cw_theme_high_contrast / cw_theme_colorblind), keyboard and gamepad focus
 * navigation of every focusable control.
 *
 * ── The accessibility tree ───────────────────────────────────────────
 * cw_a11y_snapshot walks the visible Control nodes of a tree (popups last,
 * as they draw) and describes each one an assistive technology would care
 * about: its role (from the node type), name (accessibility_name, else its
 * text, else the text of a Label right before it in the same container, as
 * form layouts pair them), value (ranges, text fields, option buttons, tabs,
 * lists), description (accessibility_description, else the tooltip),
 * states, and bounds in UI units. Items inside a control (ItemList items,
 * open PopupMenu items, TabContainer tabs) are child entries of it. Plain
 * Controls and containers without a name become GROUPs only when they hold
 * something; empty layout boxes are skipped (their children move up).
 * Text is what the user sees: translated and pseudo-localized like the
 * drawing.
 *
 * Hosts bridge this to the platform's accessibility API (AT-SPI on Linux,
 * UI Automation on Windows, NSAccessibility on macOS, the DOM/ARIA in a
 * browser). Those bridges are NOT part of the engine: they need host code
 * (and, for carts, a wasmcart ABI surface that does not exist yet). What the
 * engine provides is the data and the events, through this API and through
 * the debug channel (`a11y` command: the tree as JSON, focus, the event log,
 * announcements), which is enough for a host-side bridge to poll or follow.
 *
 * Threads: main thread only. */
#ifndef CW_A11Y_H
#define CW_A11Y_H

#include "audio.h"
#include "input.h"
#include "ui.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
  CW_ROLE_GROUP, CW_ROLE_PANEL, CW_ROLE_LABEL, CW_ROLE_BUTTON, CW_ROLE_TOGGLE_BUTTON, CW_ROLE_CHECKBOX, CW_ROLE_SWITCH,
  CW_ROLE_COMBOBOX, CW_ROLE_SLIDER, CW_ROLE_SPINBUTTON, CW_ROLE_PROGRESSBAR, CW_ROLE_TEXTFIELD, CW_ROLE_TEXTAREA,
  CW_ROLE_LIST, CW_ROLE_LISTITEM, CW_ROLE_TREE, CW_ROLE_TABLIST, CW_ROLE_TAB, CW_ROLE_MENU, CW_ROLE_MENUBAR,
  CW_ROLE_MENUITEM, CW_ROLE_DIALOG, CW_ROLE_SCROLLAREA, CW_ROLE_IMAGE, CW_ROLE_COLORPICKER, CW_ROLE_CAPTION,
  CW_ROLE_COUNT
} cw_a11y_role;

enum {
  CW_A11Y_FOCUSABLE = 1u << 0,
  CW_A11Y_FOCUSED = 1u << 1,
  CW_A11Y_DISABLED = 1u << 2,
  CW_A11Y_CHECKABLE = 1u << 3,
  CW_A11Y_CHECKED = 1u << 4,
  CW_A11Y_SELECTED = 1u << 5,
  CW_A11Y_EXPANDED = 1u << 6,  /* its popup is open */
  CW_A11Y_HAS_POPUP = 1u << 7,
  CW_A11Y_READONLY = 1u << 8,
  CW_A11Y_MULTILINE = 1u << 9,
  CW_A11Y_PASSWORD = 1u << 10,
  CW_A11Y_MODAL = 1u << 11,
  CW_A11Y_RTL = 1u << 12,      /* lays out right to left */
};

typedef struct cw_a11y_node {
  uint64_t id;            /* stable while the control lives: its node uid (items: uid * 4096 + item + 1) */
  int parent;             /* index of the parent entry in the snapshot, -1 = top level */
  cw_a11y_role role;
  uint32_t states;        /* CW_A11Y_* */
  cw_node node;           /* the control (for an item: the control holding it) */
  int item;               /* -1, or the item's index inside node */
  const char *name;       /* frame-arena or interned strings ("" when none) */
  const char *value;
  const char *description;
  cw_rect bounds;         /* UI units, canvas origin */
  double value_now, value_min, value_max; /* ranges (sliders, spin boxes, progress) */
} cw_a11y_node;

/* Describe the tree's UI into out (up to cap entries); returns the number of
 * entries the whole tree has (more than cap = truncated). Lay out first (a
 * frame does). Strings live until the end of the frame. */
int cw_a11y_snapshot(cw_tree *t, cw_a11y_node *out, int cap);
const char *cw_a11y_role_name(cw_a11y_role r); /* "button" ... (ARIA-like) */
/* The snapshot as JSON ({"nodes":[{id, parent, role, name, value, ...}]});
 * cw_alloc'd. The `a11y` debug command answers this. */
char *cw_a11y_json(cw_tree *t);

/* ── events (what a bridge forwards) ───────────────────────────────── */
typedef enum { CW_A11Y_EVENT_FOCUS, CW_A11Y_EVENT_VALUE, CW_A11Y_EVENT_ANNOUNCE } cw_a11y_event_type;
typedef struct cw_a11y_event {
  cw_a11y_event_type type;
  uint32_t seq;      /* 1, 2, 3 ... */
  cw_node node;      /* FOCUS (null = focus left the UI), VALUE */
  const char *text;  /* ANNOUNCE: the text; VALUE: the signal ("value_changed", "toggled" ...) */
  bool assertive;    /* ANNOUNCE: interrupt what is being read */
} cw_a11y_event;
typedef void (*cw_a11y_listener_fn)(const cw_a11y_event *e, void *user);
/* Called with every event as it happens (one listener; NULL removes it). */
void cw_a11y_set_listener(cw_a11y_listener_fn fn, void *user);
/* A live-region message ("Level 3", "Saved"). Captions announce themselves. */
void cw_a11y_announce(const char *text, bool assertive);
/* The last 64 events are kept for pollers: entries with seq > since, oldest
 * first; returns the count written. */
int cw_a11y_events_since(uint32_t since, cw_a11y_event *out, int cap);
uint32_t cw_a11y_event_seq(void); /* the newest event's seq */

/* ── captions and subtitles ───────────────────────────────────────────
 * A caption track (.cwcap, one cue per line, '#' comments):
 *   0.0 2.4 narrator | caption.intro.1        start end [speaker] | text
 *   2.6 5.0 | [thunder rumbles]               (no speaker: a sound caption)
 * Times are seconds; speaker and text go through cw_tr_auto (keys or plain
 * text). A playing track shows its cues while its clock is inside them: the
 * clock follows a voice's played frames (cw_voice_frames_played), or the
 * engine's frame time when the voice is null, so captions stay in sync with
 * the sound on every host. CaptionPanel (a Control) draws the active lines;
 * games may draw them themselves from cw_captions_active. Captions are on by
 * default; cw_captions_set_enabled(false) hides them (the tracks keep time). */
typedef struct cw_caption_line {
  const char *speaker; /* translated; "" for sound captions */
  const char *text;    /* translated */
  float remaining;     /* seconds left on screen */
  int track;           /* -1 for cw_captions_show lines */
} cw_caption_line;
int cw_captions_load(const char *asset_path);                      /* track id, -1 (logged) on an error */
int cw_captions_parse(const char *text, size_t len, const char *name); /* the same from memory */
/* (Re)start a track. With a voice (and its sound, for the frame rate) the
 * clock is the voice's played frames; a null voice runs on frame time. */
void cw_captions_play(int track, cw_voice voice, cw_sound sound);
/* Stop a track (its lines leave the screen); playing: it is running. */
void cw_captions_stop(int track);
bool cw_captions_playing(int track); /* false once stopped, past its last cue, or its voice ended; bad id = false */
/* A one-off line (sound effects, barks): shown for `seconds`. */
void cw_captions_show(const char *text, const char *speaker, float seconds);
int cw_captions_active(cw_caption_line *out, int cap); /* returns the count (newest last) */
/* A line's speaker as CaptionPanel shows it before the text: the catalog key
 * caption.speaker_label (a pattern with {speaker}, e.g. "{speaker} :" for
 * French) when the locale has it, else "{speaker}:"; "" for no speaker.
 * Frame-arena result. */
const char *cw_captions_speaker_label(const char *speaker);
/* The player's subtitles setting (on by default): off hides CaptionPanel's
 * lines and stops caption announcements; tracks keep their clocks. */
void cw_captions_set_enabled(bool on);
bool cw_captions_enabled(void); /* the subtitles setting */
/* Advance one frame. The UI module calls it every frame while the current
 * tree has controls; a game that draws captions itself without any UI
 * calls it from its update. */
void cw_captions_update(double dt);

/* ── input remapping ──────────────────────────────────────────────────
 * RemapButton (a Button): properties `action` (name), `slot` (binding slot)
 * and `device` (KEYBOARD_MOUSE or GAMEPAD). It shows the binding's display
 * name; pressing it listens for the next key/button/axis on that device
 * (cw_input_capture_begin), shows "remap.listening" (translated; English
 * fallback "Press a key...") and stores the capture with
 * cw_action_set_binding; Escape (or the pad's east / back button for a
 * keyboard capture) cancels. Emits remapped(string binding). */
/* A binding's human name ("Space", "Left Stick Up", "South Button"),
 * translated through the catalogs (keys input.key.<name>, input.mouse.<b>,
 * input.pad.<button>, input.axis.<axis><+|->), else English. */
const char *cw_binding_display_name(cw_binding b, char *buf, size_t cap);

#ifdef __cplusplus
}
#endif
#endif

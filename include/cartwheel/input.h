/* cartwheel input: named actions on top of the platform's raw input.
 *
 * A game names what it wants ("jump", "move_left") and binds devices to it:
 * keys (SDL scancodes), mouse buttons, the wheel, pad buttons, pad axes with
 * a direction and threshold (triggers are just axes), and on-screen touch
 * controls. Queries ask about an action for a player:
 *
 *   cw_action jump = cw_action_add("jump", 0.5f);
 *   cw_action_bind_text(jump, "key:space, pad:south");
 *   ...
 *   if (cw_action_just_pressed(0, jump)) ...
 *   cw_vec2 mv = cw_input_vector(0, left, right, up, down, CW_DEADZONE_RADIAL, 0.2f);
 *
 * ── Two contexts, one answer each ─────────────────────────────────────
 * The module keeps TWO snapshots of every action:
 *   - the TICK snapshot, taken once per fixed tick right before
 *     cw_app_desc.fixed_update. Queries made inside fixed_update read it.
 *     This is what simulation reads, what gets recorded and replayed, and
 *     what the stream hash covers.
 *   - the FRAME snapshot, taken once per rendered frame right after the
 *     platform polls events. Queries made anywhere else (update, render,
 *     event, init) read it. This is what UI reads.
 * just_pressed is correct in both:
 *   - a press that starts and ends between two ticks (a one-frame tap while
 *     the tick rate is below the frame rate, or a key released inside one
 *     frame's event batch) is LATCHED: the next tick sees it pressed and
 *     just_pressed, and the tick after sees just_released.
 *   - when several ticks run in one frame, only the FIRST of them sees
 *     just_pressed; the rest see it held.
 * A press that begins and ends inside one frame's event batch is caught by
 * the platform's per-frame "went down" bits (cw_raw_input.keys_pressed,
 * mouse_pressed, cw_pad_state.pressed, cw_touch_state.tapped).
 *
 * ── Determinism ───────────────────────────────────────────────────────
 * Every analog value is quantized to int16 steps (1/32767, the wasmcart pad
 * resolution) before it enters a snapshot, and every derived value
 * (strength, axis, vector) is computed from the quantized value with IEEE
 * + - * / sqrt only. The same action stream therefore gives bit-identical
 * answers on every host, and a recording replays exactly.
 *
 * Costs: an action query is an array read. A frame or tick snapshot walks
 * every binding of every action for every player (a few hundred compares).
 * Limits: CW_INPUT_MAX_ACTIONS actions, CW_INPUT_MAX_BINDINGS per action,
 * CW_INPUT_MAX_TOUCH_CONTROLS touch controls. Main thread only. */
/* Axis convention: cw_input_vector returns SCREEN space, x right and y DOWN
 * (stick down / S key = +y). For 3D movement on the ground plane with the
 * camera looking down -Z, use x as +X and y as +Z. */
#ifndef CW_INPUT_H
#define CW_INPUT_H

#include "base.h"
#include "math.h"
#include "platform.h"

#ifdef __cplusplus
extern "C" {
#endif

#define CW_INPUT_MAX_PLAYERS 4
#define CW_INPUT_MAX_ACTIONS 64
#define CW_INPUT_MAX_BINDINGS 8
#define CW_INPUT_MAX_TOUCH_CONTROLS 16

/* An action is an index into the action table (insertion order); -1 is
 * "no action" and every query on it returns false / 0. */
typedef int cw_action;

/* ── bindings ──────────────────────────────────────────────────────────
 * Text form (cw_binding_parse / cw_binding_format), one per binding:
 *   key:space  key:a  key:left  key:44        SDL scancode by name or number
 *   mouse:left  mouse:right  mouse:middle
 *   wheel:up  wheel:down  wheel:left  wheel:right   (pressed on frames the wheel moves)
 *   pad:south  pad:start  pad:up ...           cw_pad_button names below
 *   axis:lx-  axis:ly+  axis:rt+@0.5           pad axis, direction, optional threshold
 *   touch:0                                     touch button control 0
 *   touch:1.left  touch:1.up                    one direction of touch stick control 1
 * Pad button names: south east west north back guide start lstick rstick
 * lshoulder rshoulder up down left right. Axis names: lx ly rx ry lt rt. */
typedef enum {
  CW_BIND_NONE,
  CW_BIND_KEY,          /* code = SDL scancode */
  CW_BIND_MOUSE,        /* code = 0 left, 1 right, 2 middle */
  CW_BIND_WHEEL,        /* code = 0 vertical, 1 horizontal; dir +1 up/right, -1 down/left */
  CW_BIND_PAD_BUTTON,   /* code = cw_pad_button */
  CW_BIND_PAD_AXIS,     /* code = cw_pad_axis; dir +1 / -1; threshold */
  CW_BIND_TOUCH_BUTTON, /* code = touch control id */
  CW_BIND_TOUCH_STICK,  /* code = touch control id; dir 0 left, 1 right, 2 up, 3 down */
} cw_binding_kind;

typedef struct cw_binding {
  uint8_t kind;      /* cw_binding_kind */
  int8_t dir;
  uint16_t code;
  float threshold;   /* PAD_AXIS: deflection below this contributes 0 (0 = any) */
} cw_binding;

/* Parse one binding ("axis:lx-@0.25"). false (and *out untouched) on a typo. */
bool cw_binding_parse(const char *text, cw_binding *out);
/* Write the text form into buf (always NUL-terminated); returns buf. */
const char *cw_binding_format(cw_binding b, char *buf, size_t cap);

/* ── actions ────────────────────────────────────────────────────────── */

/* Define an action (or return the existing one with this name, updating its
 * deadzone). deadzone: raw strength at or above which the action counts as
 * pressed; strength is rescaled from deadzone..1 to 0..1. 0.5 is a good
 * default for buttons and sticks alike. Initializes the module on first use
 * (registers its debug fields and hooks into the platform's frame loop). */
cw_action cw_action_add(const char *name, float deadzone);
cw_action cw_action_find(const char *name); /* -1 when missing */
const char *cw_action_name(cw_action a); /* module-owned; NULL for an invalid action */
int cw_action_count(void); /* actions defined (0 before the first cw_action_add) */
void cw_action_set_deadzone(cw_action a, float deadzone); /* clamped to 0..1; calls the bindings-changed callback */
float cw_action_deadzone(cw_action a); /* 0 for an invalid action */

/* Rebinding. All of these call the bindings-changed callback (below).
 * bind: append (false when the action already has CW_INPUT_MAX_BINDINGS or
 * already has this binding). bind_text: append a comma-separated list. */
bool cw_action_bind(cw_action a, cw_binding b);
/* False if any item failed to parse (logged); valid items still bind. An item
 * "deadzone:<0..1>" sets the action's dead zone, as in a bindings file. */
bool cw_action_bind_text(cw_action a, const char *list);
bool cw_action_set_binding(cw_action a, int slot, cw_binding b); /* replace slot; slot == count appends */
bool cw_action_unbind(cw_action a, int slot); /* remove slot, shifting later ones down; false when out of range */
void cw_action_clear_bindings(cw_action a); /* no-op on an invalid action */
int cw_action_bindings(cw_action a, cw_binding *out, int cap); /* returns the binding count */

/* ── queries (tick snapshot inside fixed_update, frame snapshot elsewhere) ─ */

bool cw_action_pressed(int player, cw_action a);
bool cw_action_just_pressed(int player, cw_action a); /* went down this tick (or frame); see the contexts above */
bool cw_action_just_released(int player, cw_action a); /* went up this tick (or frame) */
float cw_action_strength(int player, cw_action a);     /* 0..1, rescaled past the deadzone */
float cw_action_raw_strength(int player, cw_action a); /* 0..1, before the deadzone */
/* strength(pos) - strength(neg), -1..1 */
float cw_input_axis(int player, cw_action neg, cw_action pos);

typedef enum {
  CW_DEADZONE_AXIAL,         /* each component on its own: |c| < dz -> 0, else rescaled dz..1 -> 0..1 */
  CW_DEADZONE_RADIAL,        /* length < dz -> (0,0), else the vector unchanged */
  CW_DEADZONE_SCALED_RADIAL, /* length < dz -> (0,0), else direction * rescaled length (smooth from 0) */
} cw_deadzone_mode;

/* A 2D vector from four actions' RAW strengths (action deadzones are not
 * applied; `deadzone` is): x = right - left, y = down - up (screen
 * convention, as pad sticks report it). Length is clamped to 1 in every mode. */
cw_vec2 cw_input_vector(int player, cw_action left, cw_action right, cw_action up, cw_action down,
                        cw_deadzone_mode mode, float deadzone);
/* Same, by action names (a lookup per name per call). */
cw_vec2 cw_input_vector_named(int player, const char *left, const char *right, const char *up, const char *down,
                              cw_deadzone_mode mode, float deadzone);

typedef enum { CW_INPUT_FRAME, CW_INPUT_TICK } cw_input_context;
/* Which snapshot queries read right now. The platform switches it around
 * fixed_update; set it yourself only around a manual re-simulation loop
 * (see cw_input_replay_step). Returns the previous context. */
cw_input_context cw_input_set_context(cw_input_context c);
cw_input_context cw_input_context_now(void); /* which snapshot queries read now */

/* ── players and devices ───────────────────────────────────────────────
 * Default: player 0 = pad 0 + keyboard + mouse + touch; players 1..3 = pads
 * 1..3. pad -1 = none. A device may be shared by several players. */
enum { CW_DEVICE_KEYBOARD = 1, CW_DEVICE_MOUSE = 2, CW_DEVICE_TOUCH = 4 };
/* Pad outside 0..CW_MAX_PADS-1 = none; devices = CW_DEVICE_* mask. */
void cw_input_assign(int player, int pad, uint32_t devices);
int cw_input_player_pad(int player); /* -1 when none or player out of range */
uint32_t cw_input_player_devices(int player); /* CW_DEVICE_* mask; 0 when player out of range */

/* ── rumble ────────────────────────────────────────────────────────────
 * The game drives a player's pad motors: `low` is the heavy (low-frequency)
 * motor, `high` the light one, both 0..1 (out of range is clamped, NaN is 0),
 * for `ms` milliseconds (capped at 5000). A new call replaces the one running;
 * cw_input_rumble_stop ends it early. A player with no pad, a pad without
 * motors, a keyboard-only setup and a host with no hardware all make these
 * silent no-ops, so a game can call them unconditionally and ask
 * cw_input_has_rumble only to offer the player a toggle. Natively this is
 * SDL_GameControllerRumble; in a cart it is wasmcart's wc_pad_rumble (the
 * browser's dual-rumble effect, a native player's SDL). Presentation only:
 * rumble never feeds back into input or the simulation, so it is safe to call
 * from fixed_update or update. The debug field `cw.rumble` (request count,
 * pad, low and high in thousandths, ms) shows the last request to tests. */
#define CW_HAVE_INPUT_RUMBLE 1 /* for games that also build against an older SDK */
/* Whether that player's pad has motors the host can drive (false with no pad). */
bool cw_input_has_rumble(int player);
/* Run that player's pad motors: low (heavy) and high (light) 0..1 for ms (at most 5000), replacing the one running. */
void cw_input_rumble(int player, float low, float high, int ms);
/* Stop that player's pad motors now. */
void cw_input_rumble_stop(int player);

/* ── binding sets as text ──────────────────────────────────────────────
 * One action per line, '#' starts a comment:
 *   jump = key:space, pad:south, mouse:left
 *   move_left = key:a, key:left, pad:left, axis:lx-
 *   fire = axis:rt+@0.5, key:j, deadzone:0.3
 *   special =                       # explicitly unbound
 * A line REPLACES that action's bindings (creating the action, deadzone 0.5
 * unless given). Errors are logged with the line number; valid lines still
 * apply; returns false when any line failed. Loading does not call the
 * bindings-changed callback. */
bool cw_input_load_bindings(const char *text, size_t len);
bool cw_input_load_bindings_asset(const char *path); /* cw_asset_load + load_bindings */
/* Snapshot the current bindings as the defaults (call after loading the
 * shipped defaults, before applying the user's overrides). */
void cw_input_commit_defaults(void);
void cw_input_reset_to_defaults(void); /* calls the bindings-changed callback */
/* Write bindings as text (snprintf contract: returns the length the full text
 * needs, writes at most cap-1 bytes + NUL). overrides_only: only actions that
 * differ from the committed defaults, which is what a game saves. */
size_t cw_input_save_bindings(char *dst, size_t cap, bool overrides_only);
/* Called after every rebinding call (bind/unbind/set/clear/deadzone/reset).
 * The game owns persistence: typically cw_input_save_bindings(.., true) into
 * its own save data. The engine never writes files (carts cannot). */
typedef void (*cw_bindings_changed_fn)(void *user);
void cw_input_on_bindings_changed(cw_bindings_changed_fn fn, void *user); /* one callback; NULL removes it */

/* Rebinding UI support: listen for the next thing the player presses on the
 * given devices (CW_DEVICE_* plus their pad when `pad` is true). Frame
 * context: a key/button/wheel going down, or a pad axis crossing 0.5 from
 * rest. poll returns true once with the binding; capture then stops. */
void cw_input_capture_begin(int player, uint32_t devices, bool pad);
bool cw_input_capture_poll(cw_binding *out); /* out may be NULL */
void cw_input_capture_cancel(void); /* stops listening and drops an unpolled result */
bool cw_input_capturing(void); /* still waiting for a press */

/* ── touch controls (phones, tablets, the browser) ─────────────────────
 * Regions in logical window coordinates. A touch that STARTS inside a
 * control belongs to it until lifted (sliding off a stick keeps steering).
 * Bind actions to them with touch:<id> / touch:<id>.<dir>. */
int cw_touch_add_button(float x, float y, float w, float h);
/* A stick claims touches starting in rect (x,y,w,h); its value is (touch -
 * center) / radius, length clamped to 1. floating: the center is where the
 * touch started; else the rect's center. */
int cw_touch_add_stick(float x, float y, float w, float h, float radius, bool floating);
/* Move/resize a control (layout changes); bad id ignored. */
void cw_touch_set_rect(int id, float x, float y, float w, float h);
void cw_touch_clear(void); /* remove every control */
/* Frame state for drawing the controls. */
bool cw_touch_control_active(int id);
cw_vec2 cw_touch_stick_value(int id);           /* -1..1, y down */
cw_vec2 cw_touch_stick_center(int id); /* window coordinates; (0,0) for a bad id */

/* ── record / replay ───────────────────────────────────────────────────
 * The recorded unit is the TICK snapshot (every action of every player),
 * delta-encoded: an idle tick costs well under a byte. The recording carries
 * the action names, so it replays into any build that defines them (in any
 * order; recorded actions the build lacks are dropped with a warning).
 * Actions added after record_begin are not part of that recording.
 *
 * record_begin starts at the next tick. cw_input_recording returns the
 * buffer so far (owned by the module, valid until the next
 * cw_input_recording, record_begin or input shutdown: copy it to keep it);
 * it may be called while recording continues. */
void cw_input_record_begin(void);
void cw_input_record_end(void); /* stops; the buffer stays readable via cw_input_recording */
bool cw_input_recording_active(void); /* between record_begin and record_end */
/* Buffer so far (see above); NULL and *len = 0 before any record_begin. Rebuilt on each call, so a
 * previous pointer from this function is invalidated. */
const uint8_t *cw_input_recording(size_t *len);
uint32_t cw_input_record_ticks(void); /* ticks recorded so far */
uint32_t cw_input_record_hash(void); /* hash of the recorded action stream */

/* Replay a recording (copied). While active, every platform tick takes its
 * tick snapshot from the recording instead of live devices; the replay ends
 * by itself after its last tick (the next tick is live again). false on a
 * malformed buffer (logged). */
bool cw_input_replay_begin(const uint8_t *data, size_t len);
void cw_input_replay_end(void); /* stop now and free the copy; replay ticks and hash stay readable */
bool cw_input_replay_active(void); /* a replay is feeding tick snapshots */
/* Fast re-simulation: advance the replay one recorded tick NOW, make it the
 * tick snapshot and switch queries to the tick context, so a game can run
 * its fixed step N times inside one frame. false (and the replay ends) when
 * no recorded tick is left. Restore the context afterwards with
 * cw_input_set_context(CW_INPUT_FRAME). */
bool cw_input_replay_step(void);
uint32_t cw_input_replay_ticks(void); /* recorded ticks consumed so far */
uint32_t cw_input_replay_hash(void);  /* hash of the replayed stream so far: equals the record hash at the end */

/* Hash of every tick snapshot the platform loop produced since start (live or
 * replayed). Also published as debug fields "input.hash" and "input.ticks". */
uint32_t cw_input_stream_hash(void);

/* ── driving the module ────────────────────────────────────────────────
 * The platform calls these from its frame loop (once cw_action_add has run).
 * Unit tests and tools that step input without the platform loop call them
 * directly, in this order per frame: frame_begin(raw), then tick_begin()
 * once per fixed tick, then ticks_end(). */
void cw_input_frame_begin(const cw_raw_input *raw);
void cw_input_tick_begin(void); /* take the tick snapshot (live or replayed), switch to tick context */
void cw_input_ticks_end(void); /* back to the frame context */
/* Forget all actions, controls, recordings and state (tests). */
void cw_input_reset(void);

#ifdef __cplusplus
}
#endif
#endif

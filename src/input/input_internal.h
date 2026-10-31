/* State shared by the input module's files. */
#ifndef CW_INPUT_INTERNAL_H
#define CW_INPUT_INTERNAL_H

#include "cartwheel/input.h"

enum { CW__PRESSED = 1, CW__JUST_PRESSED = 2, CW__JUST_RELEASED = 4 };

/* One action for one player in one snapshot: flags + raw strength in
 * 1/32767 steps. Everything a query returns derives from these 3 bytes. */
typedef struct cw__slot {
  uint8_t flags;
  int16_t raw;
} cw__slot;
typedef struct cw__snapshot {
  cw__slot s[CW_INPUT_MAX_PLAYERS][CW_INPUT_MAX_ACTIONS];
} cw__snapshot;

typedef struct cw__action_def {
  const char *name; /* interned */
  float deadzone;
  int16_t dz_q;
  int nbind;
  cw_binding bind[CW_INPUT_MAX_BINDINGS];
} cw__action_def;

typedef struct cw__touch_control {
  bool stick, floating;
  bool rect_home;            /* a rect stick added floating: its ring stays at a home in the rect's bottom left (rect_home_at) */
  float x, y, w, h, radius; /* the claim rect (laid out each frame for anchored ones) and the throw, window coordinates */
  /* anchored (thumbstick / round button): placed in touch units from a corner */
  bool anchored, hidden, disabled;
  uint8_t corner;            /* cw_touch_corner, AUTO resolved */
  float ux, uy, ur, uzone;   /* home / centre, radius, zone in touch units */
  cw_vec2 home;              /* laid out, window coordinates */
  char label[16];
  cw_color color;
  /* frame state */
  bool active, tapped;
  cw_vec2 value, center;
  cw_vec2 start;             /* where the touch holding a floating stick landed */
} cw__touch_control;

typedef struct cw__input_state {
  bool init;
  cw__action_def actions[CW_INPUT_MAX_ACTIONS];
  int count;
  cw__action_def defaults[CW_INPUT_MAX_ACTIONS];
  int default_count;
  int pad[CW_INPUT_MAX_PLAYERS];
  uint32_t devices[CW_INPUT_MAX_PLAYERS];
  /* snapshots */
  cw__snapshot frame, tick_live, tick_replay;
  const cw__snapshot *tick; /* what tick-context queries read */
  bool frame_held[CW_INPUT_MAX_PLAYERS][CW_INPUT_MAX_ACTIONS];
  bool pend_jp[CW_INPUT_MAX_PLAYERS][CW_INPUT_MAX_ACTIONS]; /* a press since the last tick */
  int16_t pend_peak[CW_INPUT_MAX_PLAYERS][CW_INPUT_MAX_ACTIONS];
  /* raw presses a frame saw, kept for the next tick as pend_jp keeps an action's: a frame can run no tick (a display
   * faster than the tick rate) or several (a slow one), cw_input_raw holds a frame's presses for just that frame */
  uint32_t pend_pad[CW_MAX_PADS], tick_pad[CW_MAX_PADS], pend_mouse, tick_mouse;
  uint8_t pend_keys[CW_KEY_COUNT], tick_keys[CW_KEY_COUNT];
  cw_input_context ctx;
  cw_raw_input prev_raw;
  /* touch */
  cw__touch_control controls[CW_INPUT_MAX_TOUCH_CONTROLS];
  int ncontrols;
  int8_t touch_owner[CW_MAX_TOUCHES];
  bool touch_prev_active[CW_MAX_TOUCHES];
  float area[4];     /* cw_touch_set_area; w or h <= 0 = the window */
  float unit;        /* window coordinates per touch unit, last layout */
  uint8_t overlay;   /* cw_touch_overlay_mode */
  bool touch_seen;   /* a touch landed since the pad was last used (overlay AUTO) */
  /* capture */
  struct {
    bool on, done, pad;
    int player;
    uint32_t devices;
    cw_binding result;
  } capture;
  /* bindings changed */
  cw_bindings_changed_fn changed_fn;
  void *changed_user;
  /* stream */
  uint32_t stream_hash, stream_ticks;
} cw__input_state;

extern cw__input_state cw__in;

void cw__input_ensure_init(void);
void cw__input_changed(void);
void cw__touch_update(const cw_raw_input *raw); /* touch.c: lay out, claim and steer the touch controls */
bool cw__action_bind_quiet(cw_action a, cw_binding b);
int16_t cw__input_quant(float v);
uint32_t cw__snapshot_hash(const cw__snapshot *s, int count, uint32_t h);

/* replay.c */
void cw__input_register_commands(void); /* the debug channel's record command */
void cw__input_record_tick(const cw__snapshot *s);
bool cw__input_replay_next(void); /* fills cw__in.tick_replay; false when no replay is running */
void cw__input_replay_free(void);

/* The raw presses of this tick, for code reading devices rather than actions (the game shell's join screen): each
 * press once, on one tick, whatever number of ticks its frame runs. Outside a tick, the frame's (as actions read). */
uint32_t cw__input_pad_pressed(int pad); /* a bit per CW_PAD_*; 0 for a pad not connected */
bool cw__input_key_pressed(int key);     /* a scancode */
uint32_t cw__input_mouse_pressed(void);  /* a bit per mouse button */

#endif

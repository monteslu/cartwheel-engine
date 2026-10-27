/* cartwheel motion: a command buffer and motion-input matcher for games that
 * read joystick motions and button taps (fighting games, brawlers, anything
 * with "quarter circle then punch").
 *
 * The game pushes ONE frame of input per fixed tick (cw_cmd_push) and asks
 * after each push whether a motion has just been done. Everything is integer
 * and runs inside fixed_update, so results are identical on every host and
 * replay exactly.
 *
 * Directions use numpad notation RELATIVE TO FACING:
 *     7 8 9        8 = up   2 = down   4 = back   6 = forward   5 = neutral
 *     4 5 6
 *     1 2 3
 * cw_cmd_push takes the raw left/right/up/down state and the facing (+1 =
 * facing right, -1 = facing left) and stores the relative direction. Left and
 * right together cancel; up and down together count as up.
 *
 * Motion text (cw_motion_parse):
 *   "236"     quarter circle forward: down, down-forward, forward
 *   "623"     forward, down, down-forward (rising uppercut)
 *   "214"  "41236"  "236236"  "22"  "66"   any run of up to 8 digits
 *   "!6!6"    exact steps: the direction must be exactly 6 (3 and 9 do not count): a dash
 *   "[4]6"    charge: back held for charge frames, then forward
 *   "[2]8"    charge down, then up
 * Rules (documented so a design can rely on them):
 *   - a step matches a frame when the frame's direction CONTAINS the step
 *     (step 6 matches 3, 6 and 9; step 2 matches 1, 2 and 3; step 5 only 5);
 *   - a diagonal step (1 3 7 9) is optional: "236" also accepts 2 then 6;
 *   - each step needs a frame strictly later than the one before it, and two
 *     equal steps in a row ("66", "22") need a frame in between that does not
 *     match, so a held direction is not a double tap;
 *   - the whole motion must finish inside `window` frames of its first step.
 * Buttons are a bit mask the game defines (bit i = button i).
 *
 * Match functions return how many frames ago the motion/command completed
 * (0 = this frame) or -1. Call cw_cmd_consume after a move starts so the same
 * input is not read twice. */
#ifndef CW_MOTION_H
#define CW_MOTION_H

#include "base.h"

#ifdef __cplusplus
extern "C" {
#endif

#define CW_CMD_HISTORY 128 /* frames remembered (a power of two) */
#define CW_MOTION_MAX_STEPS 8

typedef struct cw_cmd_frame {
  uint8_t dir;       /* 1..9, 5 = neutral (relative to facing) */
  uint8_t pad_;
  uint16_t buttons;  /* held mask */
} cw_cmd_frame;

typedef struct cw_cmd_buf {
  cw_cmd_frame f[CW_CMD_HISTORY];
  uint32_t count;     /* frames pushed since clear */
  uint32_t consumed;  /* frames before this index are ignored by matching */
  uint16_t prev;      /* buttons held on the previous frame */
} cw_cmd_buf;

void cw_cmd_clear(cw_cmd_buf *b);
/* One frame of input. facing: +1 right, -1 left (0 counts as right). */
void cw_cmd_push(cw_cmd_buf *b, bool left, bool right, bool up, bool down, int facing, uint16_t buttons);
/* The relative direction (1..9) for raw states; what cw_cmd_push stores. */
int cw_cmd_dir(bool left, bool right, bool up, bool down, int facing);
int cw_cmd_dir_now(const cw_cmd_buf *b); /* 5 before any push */
uint16_t cw_cmd_held_now(const cw_cmd_buf *b);
/* Frames since the button (any bit of mask) last went down, within `within`
 * frames; -1 when it did not. 0 = went down this frame. */
int cw_cmd_pressed_ago(const cw_cmd_buf *b, uint16_t mask, int within);
/* Frames since the button went up, within `within` frames; -1 otherwise. */
int cw_cmd_released_ago(const cw_cmd_buf *b, uint16_t mask, int within);
/* Forget everything pushed so far for matching (a move was taken). */
void cw_cmd_consume(cw_cmd_buf *b);

typedef struct cw_motion {
  uint8_t steps[CW_MOTION_MAX_STEPS];
  uint8_t count;
  uint8_t charge; /* the first step is a charge */
  uint8_t exact;  /* bit i: step i must be exactly that direction (written "!6") */
  uint16_t charge_frames; /* frames the charge direction must be held (default 30) */
} cw_motion;

/* false (and *out untouched) on a typo: empty, digit 0, more than 8 steps,
 * unbalanced brackets, a charge with no following step. */
bool cw_motion_parse(const char *text, cw_motion *out);
/* Frames ago the motion's last step was done, with the whole motion inside
 * `window` frames; -1 when it was not done. */
int cw_motion_match(const cw_cmd_buf *b, const cw_motion *m, int window);
/* A special move: the motion plus a button of `mask` pressed within `buffer`
 * frames (0 = this frame only). The motion may finish up to 2 frames after
 * the button went down (pressing the button first is common) and up to
 * `window` frames before it. Returns frames ago of the button press or -1. */
int cw_special_match(const cw_cmd_buf *b, const cw_motion *m, uint16_t mask, int window, int buffer);

#ifdef __cplusplus
}
#endif
#endif

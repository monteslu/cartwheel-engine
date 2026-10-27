# Motion inputs and camera framing

Sample: `motion_demo`. Gate: `unit-motion`, `unit-framing`, `motion-demo`.

Two small pieces for games that read joystick motions or keep several things in view: a command buffer with a motion matcher ([cartwheel/motion.h](header:motion.h)) and a framing camera ([cartwheel/framing.h](header:framing.h)). Both are integer or fixed-step float math only, so they are identical on every host and replay exactly.

## Command buffer

Push one frame of input per fixed tick, then ask whether a motion was just done:

```c
static cw_cmd_buf buf;
static cw_motion qcf, dp;
cw_motion_parse("236", &qcf);   /* quarter circle forward */
cw_motion_parse("623", &dp);    /* rising uppercut motion */

/* in fixed_update, per fighter */
cw_cmd_push(&buf, left, right, up, down, facing, buttons_mask);
if (cw_special_match(&buf, &qcf, BTN_PUNCH, 14, 5) >= 0) { start_fireball(); cw_cmd_consume(&buf); }
```

Directions are numpad notation relative to facing (6 forward, 4 back, 2 down, 8 up). Text forms: `236`, `623`, `41236`, `236236`, `22`, and charges `[4]6` (hold back, then forward). The rules:

- a step is satisfied by a direction that contains it (step 6 accepts 3, 6, 9), except the last step of a motion, which must be exactly its direction (so `623` is never read as `236`); write `!6` for exactly 6 (a dash is `!6!6`, so a quarter circle does not dash);
- diagonal steps are optional, so `236` also accepts down then forward;
- every step needs a later frame than the one before, and holding a diagonal is not two steps;
- equal steps in a row (`66`) need a frame between them, so a held direction is not a double tap;
- `window` is the time the whole motion may take; `buffer` is how long after the button press the move can still be read (0 = same frame). The motion may finish up to 2 frames after the button.

`cw_cmd_pressed_ago` and `cw_cmd_released_ago` answer button edges. `cw_cmd_consume` makes the matcher forget everything so far; call it when a move starts.

## Framing camera

```c
cw_frame_params p = { .yaw = 0, .pitch = -0.12f, .fov_y = 0.7f, .aspect = 16.f / 9, .margin = 0.12f, .min_dist = 5, .max_dist = 14 };
cw_frame_point pts[2] = { { ax, ay + 1, az, 0.6f }, { bx, by + 1, bz, 0.6f } };
cw_frame_result fit, cam;
cw_frame_fit(&p, pts, 2, &fit);
cw_frame_follow(&state, &p, &fit, dt, 6.0f, &cam);   /* critically damped, no overshoot */
```

`cam.pos` and `cam.target` go to the camera's position and look-at. `fit.clamped` tells you when the distance limit stopped everyone fitting. A radius keeps a body, not just its pivot, in frame.

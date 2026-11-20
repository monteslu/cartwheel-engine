/* Gameplay nodes: small node types a game would otherwise hand-roll.
 *
 * ── Timer ────────────────────────────────────────────────────────────
 * Counts down and emits `timeout`. It counts FIXED TICKS, never a clock: wait_time is turned into a whole
 * number of ticks (wait_time / dt, rounded to the nearest, at least 1), so a replay, a rollback re-simulation
 * and every host fire on the same tick. A timer runs in its node's fixed_update, in tree order, and follows the
 * node's pause mode (a paused tree stops it unless its pause mode says otherwise).
 *
 *   [node name="Spawn" type="Timer" parent="."]
 *   wait_time = 2.5
 *   autostart = true
 *
 *   wait_time   seconds between timeouts (default 1). Changing it while running keeps the ticks already counted.
 *   one_shot    stop after the first timeout (default false: repeat).
 *   autostart   start when the node becomes ready.
 *   paused      hold the count without stopping.
 *   time_left   read-only: seconds until the next timeout, 0 when stopped.
 *   running, elapsed_ticks   the live state (not saved in scenes; tree snapshots carry them, so a rollback
 *               restores a timer mid-count).
 *   signal timeout()   emitted from the timer's fixed_update. A timer started on tick T by a node that runs
 *               BEFORE it in tree order counts tick T; started by a later node (or from outside the tick) its
 *               first counted tick is T + 1. wait_time = 1 at 60 ticks a second fires 60 counted ticks later.
 *   methods start (an optional float: a new wait_time), stop.
 *
 * Cost: one integer compare per running timer per tick.
 *
 * ── VisibleOnScreenNotifier3D / VisibleOnScreenNotifier2D ────────────
 * Emits screen_entered when its box comes into view and screen_exited when it leaves: wake an enemy as the
 * player sees it, free a bullet that flew off screen.
 *   center, size   the box (3D) or rectangle (2D) in the node's space; size 2 x 2 x 2 m / 20 x 20 px
 *   on_screen      read-only: the answer as of the last fixed tick
 *   signals screen_entered(), screen_exited()
 * The test runs in the node's fixed_update. 3D: the box against the view volume of the tree's current Camera3D
 * (cw_render3d_camera: its place, fov, near and far, and the render size's aspect); walls in between do not
 * count, a box is "on screen" while it is inside the view volume. 2D: the rectangle against the view the last
 * frame drew (the current Camera2D). A hidden notifier (visible false) and one that leaves the tree are off
 * screen. A node that starts in view emits screen_entered on its first tick (2D: on the first tick after a
 * frame was drawn).
 *
 * ── SpringArm3D ──────────────────────────────────────────────────────
 * A camera boom. The arm runs from the node along its +Z for spring_length; every fixed tick a sphere of
 * `radius` (a ray at 0) is swept along it through the tree's 3D physics world, and the node's Node3D children
 * (the camera) are put at the end of the arm, or `margin` short of the first thing hit. So a camera behind a
 * character slides in toward it instead of showing the inside of a wall.
 *   spring_length, margin, radius, collision_mask
 *   exclude_parent  ignore the nearest body above the arm in the tree: the character it follows (true)
 *   extend_speed    the arm shortens at once; it grows back at this many meters a second (0 = at once)
 *   hit_length      read-only: the length now
 * Children are drawn smoothly between ticks. There is no shake of any kind: for impact use the pad's rumble.
 * Cost: one shape cast per arm per tick. */
#ifndef CW_GAMEPLAY_H
#define CW_GAMEPLAY_H

#include "math.h"
#include "scene.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Start (or restart from zero) a Timer node. wait_time > 0 also sets the property; <= 0 keeps it. No-op for a
 * node that is not a Timer. */
void cw_timer_start(cw_node timer, double wait_time);
/* Stop a Timer: no timeout until it is started again. */
void cw_timer_stop(cw_node timer);
/* true when the timer is not counting (never started, stopped, or a one_shot that fired); true for other nodes */
bool cw_timer_is_stopped(cw_node timer);
/* Seconds until the next timeout (whole ticks times the fixed dt); 0 when stopped. */
double cw_timer_time_left(cw_node timer);
/* Fixed ticks until the next timeout; 0 when stopped. The exact form of cw_timer_time_left. */
int64_t cw_timer_ticks_left(cw_node timer);

/* true while a VisibleOnScreenNotifier3D / 2D is in view (its on_screen); false for other nodes */
bool cw_notifier_is_on_screen(cw_node notifier);
/* The tests behind the notifiers, for code that wants the answer without a node. 3D: does the box (centre and
 * full size in the space of box_world) reach into the camera node's view volume? 2D: does the rectangle (in the
 * space of the 6-float transform) reach into the view the tree's last frame drew (false before one was)? */
bool cw_view3d_sees_box(cw_node camera, const t3_mat4 *box_world, t3_vec3 center, t3_vec3 size);
/* see cw_view3d_sees_box */
bool cw_view2d_sees_rect(cw_tree *t, const float *xform, cw_vec2 center, cw_vec2 size);
/* A SpringArm3D's length as of the last fixed tick (its hit_length); 0 for other nodes. */
float cw_spring_arm_length(cw_node arm);

#ifdef __cplusplus
}
#endif
#endif

/* cartwheel animation: Animation resources, the AnimationPlayer and
 * AnimationTree node types, skeletal clips from glTF, root motion, tweens.
 *
 * DETERMINISM. Everything here that runs in fixed_update (the default) is
 * bit-identical on every host: times are doubles advanced by the fixed dt,
 * interpolation uses only + - * / sqrt and cartwheel/math.h (slerp uses
 * cw_atan2/cw_sin, never libm), blending accumulates in insertion order, and
 * values are written through the reflection path (cw_node_set). Players and
 * trees keep a running FNV hash of every value they write
 * (cw_anim_player_hash / cw_animtree_hash) so a test can compare sampled
 * poses across hosts. three.c's own AnimationMixer is NOT used for sampling:
 * its slerp calls libm's sinf/atan2f. cartwheel samples three.c's keyframe
 * tracks itself and writes the bones; three.c's SkinnedMesh/Skeleton render.
 *
 * ── Animation (resource) ─────────────────────────────────────────────
 * A named clip: a length, a loop mode and tracks.
 *   value track   a node property by path relative to the player's root
 *                 node: "Arm:rotation", "Sun:energy", ":position" (the root
 *                 itself), "Arm:position.y" (one component of a vector or
 *                 colour: .x .y .z .w / .r .g .b .a). Any reflected property:
 *                 float, int, bool, vec2/3/4, quat, color, enum, string, node,
 *                 res. float/int/vec/quat/color interpolate; the rest step.
 *   bone track    "Model@bone_name:position|quaternion|scale": a three.c
 *                 object (a glTF bone) found by name under the Node3D
 *                 "Model"'s three.c object.
 *   method track  "Counter": at each key, call a method on that node: the C
 *                 callback registered under the method's name
 *                 (cw_anim_register_method), else emit the node's signal of
 *                 that name with the key's arguments.
 *   event track   keys carry a name; the player emits
 *                 animation_event(name, animation).
 * Interpolation (value and bone tracks): NEAREST holds each key until the
 * next (a step), LINEAR, CUBIC (uniform Catmull-Rom through the neighbour
 * keys), HERMITE (explicit in/out tangents per key, glTF CUBICSPLINE
 * semantics). Quaternions slerp (default) or nlerp; CUBIC/HERMITE quaternions
 * are interpolated per component and normalized. ints interpolate and round
 * half up; bool/string/node/res/enum always step.
 *
 * Key firing (method / event): a sweep from the old time to the new one fires
 * keys in (old, new] in the direction of motion, so every key fires exactly
 * once per pass. A play() from a stopped player also fires keys exactly at the
 * start time. LOOP: reaching the end fires keys at `length`, then wraps and
 * fires keys at 0 (both, at the same instant, every cycle). PING_PONG: keys at
 * the turning points fire once per turn. seek() never fires keys.
 * Time exactly at the loop end: LOOP wraps to 0, PING_PONG turns around,
 * NONE stops there and finishes.
 *
 * ── Scene files ──────────────────────────────────────────────────────
 * Animations of the players in a scene are stored after its nodes:
 *
 *   [animation name="bob" player="Anim" length=2 loop=PING_PONG]
 *   [track type=value path="Cube:position" interp=CUBIC]
 *   0 = vec3(0, 1, 0)
 *   1 = vec3(0, 2, 0)
 *   [track type=value path="Cube:quaternion" rot=NLERP]
 *   0 = quat(0, 0, 0, 1)
 *   [track type=value path="Cube:scale" interp=HERMITE]
 *   0 = vec3(1, 1, 1) in=vec3(0, 0, 0) out=vec3(1, 0, 0)
 *   [track type=method path="Counter"]
 *   0.5 = count(1, "a")
 *   [track type=event]
 *   0.25 = "step"
 *
 * One key per line: `time = value` (values as cw_value_format writes them;
 * enums as integers). `player` is the AnimationPlayer's path from the scene
 * root ("." = the root). Defaults are omitted: loop=NONE, interp=LINEAR,
 * rot=SLERP, enabled=true. Animations of players inside an instanced
 * sub-scene belong to that scene's file; animations made from glTF clips are
 * not stored (cw_anim_set_stored). Saving a loaded scene reproduces the file
 * byte for byte.
 */
#ifndef CW_ANIM_H
#define CW_ANIM_H

#include "scene.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ── Animation resources ──────────────────────────────────────────── */

CW_HANDLE_TYPE(cw_anim);

typedef enum { CW_LOOP_NONE, CW_LOOP_LINEAR, CW_LOOP_PING_PONG } cw_loop_mode;
typedef enum { CW_INTERP_NEAREST, CW_INTERP_LINEAR, CW_INTERP_CUBIC, CW_INTERP_HERMITE } cw_interp;
typedef enum { CW_ROT_SLERP, CW_ROT_NLERP } cw_rot_mode;
typedef enum { CW_TRACK_VALUE, CW_TRACK_METHOD, CW_TRACK_EVENT } cw_track_kind;
#define CW_ANIM_MAX_ARGS 4

/* A new animation with one reference (the caller's). length > 0 seconds. */
cw_anim cw_anim_new(const char *name, float length, cw_loop_mode loop);
void cw_anim_retain(cw_anim a); /* one more reference */
void cw_anim_release(cw_anim a); /* freed at zero; players hold their own reference */
bool cw_anim_valid(cw_anim a); /* false once freed */
const char *cw_anim_name(cw_anim a); /* interned; NULL when stale */
float cw_anim_length(cw_anim a); /* seconds */
void cw_anim_set_length(cw_anim a, float length); /* seconds (>= 0) */
cw_loop_mode cw_anim_loop(cw_anim a); /* its loop mode */
void cw_anim_set_loop(cw_anim a, cw_loop_mode loop); /* NONE, LINEAR or PING_PONG */
/* false: never written to scene files (clips that come from a glTF) */
void cw_anim_set_stored(cw_anim a, bool stored);

/* Tracks; each returns the new track's index. Paths as described above. */
int cw_anim_add_value_track(cw_anim a, const char *path, cw_interp interp);
int cw_anim_add_method_track(cw_anim a, const char *node_path); /* keys call methods on that node */
int cw_anim_add_event_track(cw_anim a); /* keys emit animation_event */
void cw_anim_track_set_rot_mode(cw_anim a, int track, cw_rot_mode mode); /* quaternion tracks: SLERP (default) or NLERP */
void cw_anim_track_set_enabled(cw_anim a, int track, bool enabled); /* disabled tracks are skipped */
int cw_anim_track_count(cw_anim a); /* tracks in the animation */
cw_track_kind cw_anim_track_kind(cw_anim a, int track); /* value, method or event */
const char *cw_anim_track_path(cw_anim a, int track); /* interned; "" for event tracks */
int cw_anim_track_find(cw_anim a, const char *path);  /* -1 when absent */
int cw_anim_key_count(cw_anim a, int track); /* keys on a track */
float cw_anim_key_time(cw_anim a, int track, int key); /* seconds */

/* Keys are kept sorted by time; a key at an existing time replaces it.
 * Cost: O(keys) per insert (append in time order is O(1)). */
void cw_anim_add_key(cw_anim a, int track, float time, cw_value v);
/* HERMITE tracks: tangents are value change per second (glTF CUBICSPLINE) */
void cw_anim_add_key_hermite(cw_anim a, int track, float time, cw_value v, cw_value in_tangent, cw_value out_tangent);
void cw_anim_add_method_key(cw_anim a, int track, float time, const char *method, const cw_value *args, int argc); /* up to CW_ANIM_MAX_ARGS arguments (copied) */
void cw_anim_add_event_key(cw_anim a, int track, float time, const char *name); /* an event with that name */
/* Gameplay events on any clip, a glTF one included (hit windows, footsteps, a sound cue): a key named `name` at
 * `time`, on the animation's event track (made on first use). Returns the track index, -1 for a bad handle.
 * Register a callback with cw_anim_player_on_event / cw_animtree_on_event to receive them. */
int cw_anim_add_event(cw_anim a, float time, const char *name);
int cw_anim_event_count(cw_anim a, const char *name); /* keys of that event name over the event tracks */
/* The key time (seconds) of the nth (0 first, in time order) event of that name; -1 when there is none. A game
 * can read a hit window's start and end from the clip instead of keeping a table of tick counts. */
float cw_anim_event_time(cw_anim a, const char *name, int nth);

/* The value of a value/bone track at `time` (clamped to its keys), in the
 * type of its keys. false for an empty or non-value track. Deterministic. */
bool cw_anim_sample(cw_anim a, int track, float time, cw_value *out);

/* Deterministic quaternion interpolation (xyzw arrays; out may alias). */
void cw_quat_slerp_det(float out[4], const float a[4], const float b[4], float t);
void cw_quat_nlerp(float out[4], const float a[4], const float b[4], float t); /* normalized lerp, shortest arc */

/* A skeletal animation from a three.c glTF clip: one bone track per keyframe
 * track ("<model_path>@<bone>:<prop>"), keys copied, interpolation mapped
 * (DISCRETE -> NEAREST, CUBICSPLINE -> HERMITE). Morph-weight tracks are
 * skipped (logged). The result is not stored in scene files. */
cw_anim cw_anim_from_t3_clip(const t3_animation_clip *clip, const char *model_path);

/* Method tracks call these by name before falling back to signals. */
typedef void (*cw_anim_method_fn)(cw_node target, const cw_value *args, int argc, void *user);
void cw_anim_register_method(const char *name, cw_anim_method_fn fn, void *user); /* replaces an earlier one of that name */

/* The scene-format text of one animation (see above); cw_alloc'd. */
char *cw_anim_to_text(cw_anim a, const char *player_path);
/* The other way: one [animation] section (with its [track]s and keys) into a new
 * animation, stored in scene files (the caller holds its reference). Null, logged,
 * on a parse error. The header's player= is not used (cw_anim_player_add places it). */
cw_anim cw_anim_from_text(const char *text);

/* ── AnimationPlayer (node type) ───────────────────────────────────────
 * Properties:
 *   root_node (string, "..")    track paths are relative to this node
 *   autoplay (string)           played when the player becomes ready
 *   speed_scale (float, 1)      multiplies every playback speed (may be < 0)
 *   default_blend (float, 0)    cross-fade time when play() passes blend < 0
 *   process (enum FIXED|UPDATE|MANUAL, FIXED)  which phase advances it;
 *                               MANUAL: only cw_anim_player_advance
 *   current_animation (string)  setting it plays that animation (not stored)
 *   playing (bool, read-only), position (float, read-only)
 *   root_motion_track (string)  a bone or Node3D position track whose motion
 *                               moves root_motion_target instead of the bone
 *   root_motion_target (string) a Node3D path relative to the player
 *   root_motion_lock (int, 7)   axes (bit 0 x, 1 y, 2 z, in the track's parent
 *                               space) taken out of the track and given to
 *                               the target; the rest stays on the bone
 * Signals: animation_started(name), animation_finished(name),
 *   animation_changed(old_name, new_name) (play/queue switched the current
 *   animation), animation_looped(name), animation_event(event, animation).
 * Order within a tick: advance times, write blended values, then fire the
 * method/event keys crossed and the signals, in that order. Fading-out
 * animations keep advancing but fire no keys. */

typedef enum { CW_ANIM_PROCESS_FIXED, CW_ANIM_PROCESS_UPDATE, CW_ANIM_PROCESS_MANUAL } cw_anim_process;

/* Library: name -> animation. The player retains it; a name already present
 * is replaced. */
void cw_anim_player_add(cw_node player, const char *name, cw_anim a);
void cw_anim_player_remove(cw_node player, const char *name); /* releases the player's reference */
cw_anim cw_anim_player_get(cw_node player, const char *name); /* null handle when absent */
int cw_anim_player_count(cw_node player); /* animations in the library */
const char *cw_anim_player_name_at(cw_node player, int i); /* library order; NULL past the end */

/* Play `name` (NULL = resume the current one). blend < 0 uses the pair's
 * blend time (cw_anim_player_set_blend) or default_blend. Playing the
 * animation that is already current only resumes it. */
void cw_anim_player_play(cw_node player, const char *name, float blend);
/* speed multiplies speed_scale (negative plays backwards); from_end starts at
 * the end (backwards playback). */
void cw_anim_player_play_ex(cw_node player, const char *name, float blend, float speed, bool from_end);
void cw_anim_player_pause(cw_node player); /* keeps the position; play(NULL) resumes */
/* keep_state: leave the position (play() resumes); else rewind to 0. Values
 * stay as last written either way. Clears the queue. */
void cw_anim_player_stop(cw_node player, bool keep_state);
/* Jump to `time` in the current animation; update: write the values now. */
void cw_anim_player_seek(cw_node player, float time, bool update);
/* Play `name` when the current (non-looping) animation finishes. */
void cw_anim_player_queue(cw_node player, const char *name);
void cw_anim_player_clear_queue(cw_node player); /* drops queued animations */
/* Cross-fade time from one animation to another ("*" matches any). */
void cw_anim_player_set_blend(cw_node player, const char *from, const char *to, float seconds);
/* Advance by dt seconds now (any process mode). */
void cw_anim_player_advance(cw_node player, double dt);
/* Event callback (C): called for every event key the player crosses, in time order, just before the
 * animation_event signal, with the event's name, the animation's name and the key's time in the clip. It runs
 * inside the tick (fixed_update by default), so a hit window opened by a key is open for the same tick's
 * gameplay. Fading-out animations fire none. One callback per player; NULL removes it. The callback may free
 * the player. */
typedef void (*cw_anim_event_fn)(cw_node player, const char *event, const char *animation, float key_time, void *user);
/* Set the player's event callback (cw_anim_event_fn above) and the `user` pointer it gets; NULL removes it. */
void cw_anim_player_on_event(cw_node player, cw_anim_event_fn fn, void *user);
/* Animation level of detail for characters far from the camera: the player samples its clip (and writes
 * the bones) only on every divisor-th advance, with all the time since the last sample, so the pose steps
 * and an event key fires up to divisor-1 ticks late, but the clip's time, loop count and the number of events
 * come out exactly as at full rate. 1 (default) is every
 * tick; at most 64. Players are spread over the ticks by their slot, so a crowd does not sample in
 * lock step. Deterministic. Cross-fades see the coarser steps too. */
void cw_anim_player_set_update_divisor(cw_node player, int divisor);
int cw_anim_player_update_divisor(cw_node player); /* 1 when at full rate */
/* The divisor for a character `distance` from the camera: 1 up to full_rate_distance, then 2, 4, 8 each time the
 * distance doubles (never more than 8). */
int cw_anim_lod_divisor(float distance, float full_rate_distance);
const char *cw_anim_player_current(cw_node player); /* NULL when none */
bool cw_anim_player_is_playing(cw_node player); /* advancing now */
double cw_anim_player_position(cw_node player); /* seconds into the current animation */
/* running hash of every value this player wrote (and its root motion) */
uint32_t cw_anim_player_hash(cw_node player);
/* root motion produced by the last tick, in the target's parent space */
t3_vec3 cw_anim_player_root_motion(cw_node player);

/* ── AnimationTree (node type) ─────────────────────────────────────────
 * A blend graph evaluated every tick into the same mixer a player uses.
 * Properties: player (string path to the AnimationPlayer whose library and
 *   root_node it uses; its own playback should be stopped), active (bool),
 *   process (enum, as the player), root_motion_track / root_motion_target /
 *   root_motion_lock (as the player).
 * Graph nodes (ids returned below) are owned by the tree; each has at most
 * one parent. Every node evaluated in a tick advances its time, even at
 * weight 0 (blend-space children stay in phase).
 * Parameters are named floats (bools are 0/1) that blend spaces and
 * transition conditions read. Signals: state_changed(machine_state_name)
 * when any state machine enters a state. */

typedef int cw_blend_id; /* -1 = none */

cw_blend_id cw_animtree_clip(cw_node tree, const char *anim_name); /* a leaf playing a library animation */
/* playback speed of a clip node (default 1) */
void cw_animtree_clip_set_speed(cw_node tree, cw_blend_id clip, float speed);
/* 1D blend space: children at positions on `param`; the two around the value
 * blend linearly, outside the ends the end child is alone. */
cw_blend_id cw_animtree_blend1d(cw_node tree, const char *param);
void cw_animtree_blend1d_add(cw_node tree, cw_blend_id space, cw_blend_id child, float pos); /* child at pos on the parameter axis */
/* 2D blend space (gradient-band interpolation: weights fall off along each
 * pair of points; exact at a point, smooth between, no triangulation). */
cw_blend_id cw_animtree_blend2d(cw_node tree, const char *param_x, const char *param_y);
void cw_animtree_blend2d_add(cw_node tree, cw_blend_id space, cw_blend_id child, float x, float y); /* child at (x, y) */
/* lerp(a, b, param) */
cw_blend_id cw_animtree_blend2(cw_node tree, cw_blend_id a, cw_blend_id b, const char *param);
/* base + additive * param. The additive branch adds each value's difference
 * from the binding's rest value (the value it had when first animated: for
 * glTF bones, the bind pose); quaternions compose rest^-1 * sample. */
cw_blend_id cw_animtree_add(cw_node tree, cw_blend_id base, cw_blend_id additive, const char *param);
/* Restrict a subtree to these bones / node paths (a bone mask). Bone tracks
 * match by bone name, value tracks by their node path. */
void cw_animtree_set_filter(cw_node tree, cw_blend_id node, const char *const *names, int count);
void cw_animtree_set_root(cw_node tree, cw_blend_id node); /* default: the first node created, or what adopted it */

/* Event callback of a tree (as cw_anim_player_on_event): the event keys of the clips it plays. */
void cw_animtree_on_event(cw_node tree, cw_anim_event_fn fn, void *user);

/* State machines */
typedef enum { CW_ADVANCE_AUTO, CW_ADVANCE_ENABLED, CW_ADVANCE_DISABLED } cw_advance_mode;
typedef enum { CW_SWITCH_IMMEDIATE, CW_SWITCH_AT_END } cw_switch_mode;
typedef struct cw_transition {
  float xfade;               /* cross-fade seconds; while a cross-fade runs no
                              * other transition is evaluated, so keep fades
                              * short where chains of transitions matter */
  cw_advance_mode advance;   /* AUTO: taken by itself when the condition holds;
                              * ENABLED: only by travel; DISABLED: never */
  cw_switch_mode switch_mode;/* AT_END: waits for the current state's end
                              * (a non-looping clip finishing, a looping one
                              * wrapping) */
  const char *condition;     /* "speed > 0.5", "grounded", "!grounded", "dir == 2";
                              * ops == != < <= > >=; NULL = always */
} cw_transition;
cw_blend_id cw_animtree_state_machine(cw_node tree); /* an empty state machine node */
/* A state plays a graph node; the first state added is the start state. */
void cw_animtree_sm_add_state(cw_node tree, cw_blend_id sm, const char *state, cw_blend_id node);
void cw_animtree_sm_add_transition(cw_node tree, cw_blend_id sm, const char *from, const char *to, const cw_transition *t); /* t NULL: AUTO, IMMEDIATE, no fade, no condition */
/* Jump to a state now (no fade). */
void cw_animtree_sm_start(cw_node tree, cw_blend_id sm, const char *state);
/* Walk to `state` along the shortest chain of transitions (breadth first,
 * transitions in insertion order, DISABLED ones excluded), taking each with
 * its fade and switch mode; auto-advance is suspended meanwhile. No path:
 * jumps there directly. */
void cw_animtree_sm_travel(cw_node tree, cw_blend_id sm, const char *state);
const char *cw_animtree_sm_current(cw_node tree, cw_blend_id sm); /* interned */
const char *cw_animtree_sm_next(cw_node tree, cw_blend_id sm);    /* in a transition, else NULL */

void cw_animtree_set_param(cw_node tree, const char *name, float value); /* created on first set */
float cw_animtree_get_param(cw_node tree, const char *name); /* 0 when never set */
/* Advance by dt now (any process mode). */
void cw_animtree_advance(cw_node tree, double dt);
uint32_t cw_animtree_hash(cw_node tree); /* running hash of every value the tree wrote */
t3_vec3 cw_animtree_root_motion(cw_node tree); /* last tick's motion, target parent space */
/* the weight the last evaluation gave a clip node (0..1, after all blends) */
float cw_animtree_clip_weight(cw_node tree, cw_blend_id clip);

/* ── Tweens (handles, not nodes) ───────────────────────────────────────
 * A tween is a sequence of steps; steps added after cw_tween_parallel()
 * join the previous step's group, and groups run one after another. A
 * property step captures its start value when it starts (or uses
 * cw_tween_from). Leftover time carries into the next step, so a sequence
 * lands on the same tick on every host and at every frame rate (fixed mode).
 * Tweens run after the tree's nodes in fixed_update (default) or update,
 * pause with the tree (unless bound to a node that processes while paused:
 * pause_mode ALWAYS or WHEN_PAUSED, as a pause menu's nodes are), and die
 * with the node they are bound to. A finished
 * tween frees itself (its handle reads as stale). */

CW_HANDLE_TYPE(cw_tween);
typedef void (*cw_tween_fn)(cw_tween tw, void *user);
typedef enum { CW_TWEEN_FIXED, CW_TWEEN_UPDATE } cw_tween_process;

/* An empty tween on a tree (NULL = the current tree); it starts on the next
 * phase that runs it. */
cw_tween cw_tween_new(cw_tree *tree);
/* Convenience: a tween on n's tree, bound to n, with one property step. */
cw_tween cw_tween_property(cw_node n, const char *prop, cw_value to, float duration, cw_ease ease);
/* Steps (appended to the sequence; see cw_tween_parallel) */
void cw_tween_add_property(cw_tween tw, cw_node n, const char *prop, cw_value to, float duration, cw_ease ease);
void cw_tween_add_interval(cw_tween tw, float seconds); /* a wait */
/* called once when the step starts; the callback may free nodes
 * (cw_node_free is deferred while the tree processes) and kill tweens */
void cw_tween_add_callback(cw_tween tw, cw_tween_fn fn, void *user);
/* modifiers of the LAST added step */
void cw_tween_from(cw_tween tw, cw_value from);
void cw_tween_relative(cw_tween tw); /* `to` is an offset from the start value */
void cw_tween_delay(cw_tween tw, float seconds); /* seconds before the step starts, within its group */
/* the next step runs alongside the last one */
void cw_tween_parallel(cw_tween tw);
/* 0 = forever (a sequence that takes no time stops after one pass a tick) */
void cw_tween_set_loops(cw_tween tw, int loops);
void cw_tween_set_speed(cw_tween tw, float speed); /* time scale (default 1) */
void cw_tween_set_process(cw_tween tw, cw_tween_process mode); /* FIXED (default) or UPDATE */
void cw_tween_bind(cw_tween tw, cw_node n); /* killed when n is freed */
void cw_tween_on_finished(cw_tween tw, cw_tween_fn fn, void *user); /* after the last loop (not on kill) */
void cw_tween_pause(cw_tween tw); /* stops advancing */
void cw_tween_play(cw_tween tw); /* resumes */
void cw_tween_kill(cw_tween tw); /* no finished callback */
bool cw_tween_running(cw_tween tw); /* valid and not paused */
int cw_tween_loops_done(cw_tween tw); /* completed passes */
/* Advance by dt now, outside the tree's phases (tests, MANUAL use). */
void cw_tween_step(cw_tween tw, double dt);
/* live tweens on a tree */
int cw_tween_count(cw_tree *tree);

#ifdef __cplusplus
}
#endif
#endif

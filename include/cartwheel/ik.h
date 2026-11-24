/* cartwheel inverse kinematics: a pose layer between animation and
 * skinning, deterministic solvers, and skeleton modifier nodes that adjust
 * the animated pose before it is drawn (limbs that reach and plant, hair
 * and coat tails that swing).
 *
 * PIPELINE (each fixed tick, for every model with modifier nodes):
 *   1. AnimationPlayer / AnimationTree write the animated pose onto the
 *      model's bones (cartwheel/anim.h), in the nodes' fixed_update.
 *   2. The pose layer captures the bones' local translation, rotation and
 *      scale into a cw_pose (the animated pose), after every node's
 *      fixed_update and after the physics step.
 *   3. The model's modifier nodes (its children: TwoBoneIK3D,
 *      LookAtModifier3D, CCDIK3D, FABRIK3D, FootPlacement3D, SpringBone3D)
 *      run in child order, each adjusting the pose the previous one left.
 *   4. The pose is written back to the bones once; three.c skins from them.
 * Every tick starts again from the animated pose, never from the last
 * tick's solution: a bone the animation did not write this tick (a static
 * model, a bone no track drives) gets its last animated value back before
 * the modifiers run, so solutions never accumulate. A bone written by
 * anything else after the last flush (a player in UPDATE mode, game code)
 * keeps that value as its animated value.
 *   5. The app's own fixed_update runs after the modifiers. A model whose
 *      bones it writes there (cw_animtree_advance or cw_anim_player_advance
 *      of a MANUAL tree or player, bones posed by code) is solved again
 *      right after it, the same tick (cw_skeleton_modifiers_refresh, which
 *      the scene runner calls): the modifiers adjust the pose the app left,
 *      with its targets where it moved them. Foot placement's easing and
 *      SpringBone3D's tails advance once a tick however often a model
 *      solves in it. A model whose bones the app leaves alone is not solved
 *      again: targets moved there apply on the next tick.
 * SpringBone3D is the one modifier with state of its own: its simulated
 * tails carry over from tick to tick (that is the lag), but it too starts
 * each tick from the animated pose and pulls its chains back toward it.
 *
 * MODEL SPACE. A pose works in its model's space: the space of the three.c
 * object the pose was bound to (a Node3D's object holding a glTF scene).
 * Bones whose parent is not a bone are placed by the objects between them
 * and that root (a glTF armature node and its scale). Solvers take targets
 * in model space; nodes convert world targets with the model's global
 * transform. Rotations are composed as quaternions, so scale along the chain
 * must be uniform (glTF armatures are).
 *
 * DETERMINISM. Solvers use + - * / and sqrt, cartwheel/math.h's cw_acos /
 * cw_sincos where an angle is needed (r187 CCD, angle limits), fixed
 * iteration counts and exact 1/sqrt: the same input pose gives the same
 * output bits on every host. Each model's pose hash (cw_skeleton_hash)
 * folds every flushed value, so tests compare poses across hosts.
 *
 * THREADING. Captures, target resolution and FootPlacement3D's raycasts
 * (one cw_world3d_raycast_batch for every leg of every model) run on the
 * main thread; the solves run per model through cw_parallel_for. Each
 * model's work touches only its own pose, so the result does not depend on
 * the worker count.
 *
 * COST (890M laptop, native, one character with feet, a hand and a head
 * look-at): see docs/guides/animation.md "Inverse kinematics". A two-bone
 * solve is about 0.1 us; a pose capture and flush is O(bones).
 * SpringBone3D: about 3 us a chain in a release cart (3 to 4 joints, five
 * shapes; "Hair, tails and coats" in the same guide); a robe in skirt mode
 * (eight chains of four bones, six shapes) is measured in "Robes, skirts and
 * long coats" there.
 */
#ifndef CW_IK_H
#define CW_IK_H

#include "scene.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ── poses ─────────────────────────────────────────────────────────── */

/* A skeleton pose: per bone a parent index, local translation, rotation and
 * scale (struct-of-arrays), the rest pose, and model-space transforms. Bones
 * are ordered parents before children. The arrays are owned by the pose
 * (cw_pose_free); a pose bound to three.c objects (cw_pose_bind) also points
 * at them (not retained: keep the model alive while the pose is used). */
typedef struct cw_pose {
  int count;
  int *parent;           /* parent bone index; -1: placed by base[i] */
  const char **name;     /* bone names (the three.c objects' names; NULL in a free pose) */
  t3_object **bone;      /* bound objects; NULL entries in a free pose */
  t3_vec3 *pos;          /* local translation */
  t3_quat *rot;          /* local rotation (unit) */
  t3_vec3 *scl;          /* local scale */
  t3_vec3 *rest_pos;     /* local transform at bind (cw_pose_bind / cw_pose_set_rest) */
  t3_quat *rest_rot;
  t3_vec3 *rest_scl;
  t3_mat4 *base;         /* parent < 0: model space -> the bone's parent space (identity in a free pose) */
  t3_quat *base_rot;     /* base's rotation */
  t3_mat4 *model;        /* model-space matrix (cw_pose_update_model) */
  t3_quat *mrot;         /* model-space rotation */
  t3_object *root;       /* the object model space belongs to (NULL: free pose) */
} cw_pose;

/* A free-standing pose of `count` bones: parents -1, identity transforms,
 * identity bases (unit tests, procedural rigs). Set parent[], pos[], rot[],
 * scl[] directly, then cw_pose_update_model. Parents must come first. */
void cw_pose_init(cw_pose *p, int count);
/* Bind to every bone (T3_BONE) under `root` and every non-mesh object
 * hanging off a bone (glTF "_end" markers, sockets), depth first. Captures
 * the current transforms as both the pose and the rest pose. false (logged)
 * when there are none. */
bool cw_pose_bind(cw_pose *p, t3_object *root);
void cw_pose_free(cw_pose *p); /* frees the arrays; zeroes the struct */
int cw_pose_find(const cw_pose *p, const char *name); /* bone index by name, -1 when absent; O(bones) */
/* bones -> pose (local TRS, and the bases of parentless bones); then model transforms */
void cw_pose_capture(cw_pose *p);
/* pose -> bones (local TRS); no-op for a free pose */
void cw_pose_flush(const cw_pose *p);
/* the current local transforms become the rest pose */
void cw_pose_set_rest(cw_pose *p);
/* Model transforms of every bone (O(bones)), or of `bone` and its
 * descendants (O(bones after it)). Solvers keep them current. */
void cw_pose_update_model(cw_pose *p);
void cw_pose_update_from(cw_pose *p, int bone); /* the bone and everything below it */
t3_vec3 cw_pose_model_position(const cw_pose *p, int bone); /* model-space origin of a bone (zero when out of range) */
t3_quat cw_pose_model_rotation(const cw_pose *p, int bone); /* model-space rotation (identity when out of range) */
/* is `bone` `ancestor` or below it */
bool cw_pose_is_descendant(const cw_pose *p, int bone, int ancestor);
/* Turn a bone by a model-space rotation about its own origin (its model
 * rotation becomes delta * rotation); descendants follow. */
void cw_pose_rotate_model(cw_pose *p, int bone, t3_quat delta);
/* Give a bone this model-space rotation; descendants follow. */
void cw_pose_set_model_rotation(cw_pose *p, int bone, t3_quat q);
/* Move a bone by a model-space offset; descendants follow. */
void cw_pose_translate_model(cw_pose *p, int bone, t3_vec3 delta);
/* Blend each bone's local transform toward `to` (same skeleton) by w:
 * translation and scale lerp, rotation slerps (deterministic). */
void cw_pose_blend(cw_pose *p, const cw_pose *to, float w);
/* Copy local transforms (same bone count) */
void cw_pose_copy_locals(cw_pose *dst, const cw_pose *src);
/* fold every local transform into h (FNV-1a over the float bits) */
uint32_t cw_pose_hash(const cw_pose *p, uint32_t h);

/* Deterministic quaternion helpers the solvers use (exposed for game code
 * that builds targets): the shortest rotation taking unit vector `from` to
 * unit vector `to` (no trig), and a rotation about a unit axis (cw_sincos). */
t3_quat cw_quat_from_to(t3_vec3 from, t3_vec3 to);
/* A rotation of `angle` radians about the unit `axis` (cw_sincos: deterministic). */
t3_quat cw_quat_axis_angle_det(t3_vec3 axis, float angle);

/* ── solvers ───────────────────────────────────────────────────────── */

/* Two-bone IK (arms, legs): bones a (upper), b (middle), c (end) with b
 * below a and c below b. Places c at `target` (model space) when it is in
 * reach, else stretches the chain toward it. The middle joint bends in the
 * plane through a, the target and the pole. Trig-free: the middle joint's
 * position comes from the law of cosines, each bone turns by the shortest
 * rotation onto its new direction. */
typedef struct cw_ik2_opts {
  t3_vec3 pole;          /* model-space point the middle joint bends toward (use_pole) */
  bool use_pole;         /* false: keep the bend plane of the incoming pose */
  float soften;          /* 0..1: reach eases off from soften * length to full length
                          * (no snap at full extension); >= 1 or <= 0: off */
  float weight;          /* blend with the incoming pose (1 = solved) */
  bool use_end_rotation; /* give c the model rotation end_rotation */
  t3_quat end_rotation;
} cw_ik2_opts;
#define CW_IK2_OPTS_DEFAULT ((cw_ik2_opts){ .pole = { 0, 0, 0 }, .soften = 1, .weight = 1, .end_rotation = { 0, 0, 0, 1 } })
/* true when the target was in reach (within the chain's length). NULL o =
 * CW_IK2_OPTS_DEFAULT. */
bool cw_ik_two_bone(cw_pose *p, int a, int b, int c, t3_vec3 target, const cw_ik2_opts *o);

/* Aim (look-at): turn `bone` so its local `forward` axis points from the
 * bone (plus `offset`) at `target` (model space), by the shortest rotation
 * (no roll). The turn is limited to `limit_angle` radians away from the
 * incoming pose's forward direction (a head that cannot look behind
 * itself). */
typedef struct cw_aim_opts {
  t3_vec3 forward;       /* the bone's local forward axis (unit) */
  t3_vec3 offset;        /* aim origin offset in the bone's local frame (eyes in front of the joint) */
  float limit_angle;     /* radians; <= 0: unlimited */
  float weight;          /* blend with the incoming pose */
} cw_aim_opts;
#define CW_AIM_OPTS_DEFAULT ((cw_aim_opts){ .forward = { 0, 0, 1 }, .offset = { 0, 0, 0 }, .limit_angle = 0, .weight = 1 })
/* true when the target is within the limit (the bone points right at it) */
bool cw_ik_aim(cw_pose *p, int bone, t3_vec3 target, const cw_aim_opts *o);

/* CCD, exactly three.js r187's CCDIKSolver.updateOne/update, on a pose:
 * - the target is the target bone's model position (or target_position
 *   when target < 0), read once per chain;
 * - links run in array order (r187: effector side first); a disabled link
 *   ends the pass;
 * - the angle is acos(clamp(dot)), computed as atan2(|cross|, dot) (the
 *   same angle; float acos near 1 would lose half its bits where three.js
 *   has doubles); turns under 1e-5 are skipped;
 *   min_angle / max_angle clamp the step; the rotation is about
 *   cross(effector, target) in the link's own frame, applied as
 *   link.quaternion * step;
 * - `limitation` projects the rotation onto that axis keeping its sign,
 *   then rotation_min / rotation_max clamp its XYZ Euler angles;
 * - the iterations stop early when no link turned;
 * - with a blend factor < 1 every link slerps from its incoming rotation.
 * Mapping from r187: ik.target -> target, ik.effector -> effector,
 * ik.links[j] -> links[j] (index, enabled: !disabled, limitation,
 * rotationMin, rotationMax), ik.iteration, ik.minAngle, ik.maxAngle,
 * ik.blendFactor. */
typedef struct cw_ccdik_link {
  int index;
  bool disabled;         /* r187 enabled === false */
  bool has_limitation;
  t3_vec3 limitation;    /* unit axis in the link's frame */
  bool has_rotation_min, has_rotation_max;
  t3_vec3 rotation_min, rotation_max; /* XYZ Euler radians */
} cw_ccdik_link;
typedef struct cw_ccdik {
  int target;            /* bone index; < 0: target_position */
  t3_vec3 target_position;
  int effector;
  const cw_ccdik_link *links;
  int link_count;
  int iteration;         /* <= 0: 1 (r187 default) */
  bool has_min_angle, has_max_angle;
  float min_angle, max_angle;
  bool has_blend_factor;
  float blend_factor;    /* overrides the solve's blend for this chain */
} cw_ccdik;
/* r187 CCDIKSolver.update(blend): each chain in order. */
void cw_ccdik_solve(cw_pose *p, const cw_ccdik *iks, int n, float blend);

/* FABRIK over a chain of bones (tails, spines, tentacles, cables): chain[0]
 * is the root (its position stays), chain[n-1] the end; each is below the
 * previous. Joints keep their incoming lengths. Per-joint constraints
 * (optional, n entries): a cone (the bone stays within half_angle of its
 * parent's direction) or a hinge (the bone stays in the plane normal to
 * `axis`, given in the incoming pose's frame of the joint's parent bone).
 * Positions become rotations by the shortest turn per bone, so FABRIK adds
 * no roll. Stops after max_iterations or when the end is within
 * min_distance. */
typedef enum { CW_IK_JOINT_FREE, CW_IK_JOINT_CONE, CW_IK_JOINT_HINGE } cw_ik_joint_kind;
typedef struct cw_ik_joint {
  cw_ik_joint_kind kind;
  t3_vec3 axis;          /* HINGE: axis in the parent bone's local frame */
  float half_angle;      /* CONE: radians */
} cw_ik_joint;
typedef struct cw_chain_opts {
  int max_iterations;    /* default 4 */
  float min_distance;    /* model units; default 0.001 */
  float weight;          /* blend with the incoming pose */
  const cw_ik_joint *joints; /* n entries or NULL; joints[0] is the root's own */
} cw_chain_opts;
#define CW_CHAIN_OPTS_DEFAULT ((cw_chain_opts){ .max_iterations = 4, .min_distance = 0.001f, .weight = 1 })
#define CW_IK_MAX_CHAIN 64 /* longest chain cw_ik_fabrik and the CCD links take */
/* Returns the iterations run (0: already there). n from 2 to CW_IK_MAX_CHAIN. */
int cw_ik_fabrik(cw_pose *p, const int *chain, int n, t3_vec3 target, const cw_chain_opts *o);

/* ── skeleton modifier nodes ───────────────────────────────────────────
 * Children of the Node3D that holds a skinned model (its three.c object has
 * the bones). They run in child order after animation
 * (see PIPELINE). Common properties (SkeletonModifier3D, the base type):
 *   active (bool, true)       skipped when false
 *   influence (float, 1)      blend of this modifier's result with the pose
 *                             it was given (0..1); animate or tween it to
 *                             blend in and out
 * Bone properties are bone names; targets are node paths relative to the
 * modifier (a Node3D: its global transform), else the world-space
 * *_position property. "Rest" below is the skin's bind pose (from its
 * inverse bind matrices; a bone no skin lists keeps its transform at bind
 * under its parent's bind pose), whatever the bones hold when the
 * modifiers first run: a game may build the model, its AnimationTree and
 * its modifiers and advance the tree before the first modifier phase.
 *
 * TwoBoneIK3D        root_bone, middle_bone, end_bone, target,
 *                    target_position (vec3 world), pole (node path),
 *                    pole_position (vec3 world, used when use_pole and no
 *                    pole node), use_pole (bool), soften (float 0..1),
 *                    use_target_rotation (bool: the end bone takes the
 *                    target node's world rotation times target_rotation_offset,
 *                    a quat in the target's frame: grip a handle, hold a
 *                    wheel); reached (bool, read-only).
 * LookAtModifier3D   bone, target, target_position, forward_axis (vec3, in
 *                    the bone's rest frame, model space: the way the bone
 *                    faces at rest, e.g. (0,0,1) for a glTF character's
 *                    head), offset (vec3, model rest space), limit_angle
 *                    (radians, 0 = none); in_limit (bool, read-only). Chain
 *                    two (neck at influence 0.4, then head) to spread a turn.
 * CCDIK3D            root_bone, end_bone, target, target_position,
 *                    max_iterations (int, 4), min_angle / max_angle
 *                    (radians; 0 = none); per joint limits through
 *                    cw_ccdik3d_set_joint. The links run from the bone
 *                    above end_bone up to root_bone, as r187 lists them.
 * FABRIK3D           root_bone, end_bone, target, target_position,
 *                    max_iterations (int, 4), min_distance (float, 0.001);
 *                    per joint constraints through cw_fabrik3d_set_joint.
 * FootPlacement3D    legs (string: "upper,lower,end[,foot]" per leg, legs
 *                    separated by ';'), pelvis (bone moved down so the
 *                    lowest foot reaches), ray_up (float m, 0.5: rays start
 *                    this far above the animated ankle), max_step_down
 *                    (float m, 0.5), foot_height (float m, 0.1: ankle above
 *                    the sole), max_slope (radians, 0.7: foot alignment
 *                    limit), blend_speed (float 1/s, 12: how fast offsets
 *                    follow the ground), collision_mask (int, all).
 *                    Per leg it casts a ray down through the animated
 *                    ankle (Box3D, the tree's physics world), lifts or
 *                    lowers the ankle by the ground's height relative to
 *                    the model's origin (keeping the animation's swing
 *                    height), lowers the pelvis when a foot must reach
 *                    down, solves the leg as two-bone IK, carries a foot
 *                    bone that is not below the chain along with the
 *                    ankle, and turns the foot onto the ground normal
 *                    while it is near the ground. A leg whose ray misses
 *                    blends back to its animation.
 * SpringBone3D       secondary motion for hair, braids, ponytails, beards,
 *                    tails, coat tails and robe panels: skinned parts that
 *                    lag behind the body, sway and settle back to the
 *                    animated pose. Its children say what swings and what
 *                    it must not go through:
 *                    SpringBoneChain3D children, one per chain: root_bone,
 *                      end_bone (below root_bone; its child bone, a glTF
 *                      "_end" marker, or else its own length again along
 *                      the incoming direction is the tip), stiffness (1/s^2,
 *                      120: the pull back toward the animated pose; a
 *                      chain sways at about sqrt(stiffness) / 2 pi Hz),
 *                      damping (1/s, 8: drag on the swing), gravity (vec3 world
 *                      direction, down) and gravity_strength (m/s^2, 0:
 *                      the chain sags about strength / stiffness metres),
 *                      radius (m, 0.02: each joint's and the tip's hit
 *                      radius), max_angle (radians, 2: the most a joint
 *                      swings off its animated direction, so a chain never
 *                      flips over its bone; 0 no limit); damping near
 *                      critical, about 2 sqrt(stiffness), keeps a short
 *                      stiff chain from building up on a run's bob; tip_offset (vec3 world m) and tip_distance
 *                      read-only: the tip from where the animated pose
 *                      puts it, last tick.
 *                    SpringBoneCollision3D children: bone, shape (SPHERE,
 *                      CAPSULE), radius (m), offset and tail (vec3 m from
 *                      the bone's origin in the model's rest orientation:
 *                      a sphere's centre, a capsule's two ends). Every
 *                      chain of the node is kept out of every shape.
 *                    Properties: collisions (bool, true), reset (bool: set
 *                    true and the chains start again from the animated
 *                    pose next tick; it clears itself; also
 *                    cw_spring_bone3d_reset), teleport_distance (m, 0.25:
 *                    under a controller's step-up, over a tick's run) and
 *                    teleport_angle (radians, 1.5): a model that moves or
 *                    turns further than this in one tick teleported, and
 *                    the chains move with it as they hang, with no whip (0
 *                    turns the check off); resets (int, read-only).
 *                    Each joint's tail is a Verlet point kept in world
 *                    space from tick to tick: the model's motion leaves it
 *                    behind, the spring pulls it toward where the
 *                    animation puts it below the already simulated parent,
 *                    gravity sags it, the bone turns at most max_angle off
 *                    its animated direction and out of the shapes along
 *                    the sphere of its length (length and shapes both
 *                    hold), then the bone turns by the shortest rotation
 *                    onto it. A node that missed a tick
 *                    (inactive, just added) starts again from the animated
 *                    pose. Arithmetic, sqrt and cw_exp / cw_cos only, so
 *                    every host gives the same bits.
 *                    Robes, skirts and long coats: skirt (enum OFF, RING,
 *                      OPEN) links the chains, in child order, side to
 *                      side at every joint and solves them together: RING
 *                      also links the last to the first (a robe), OPEN
 *                      leaves the front open (a coat from one front edge
 *                      round the back to the other). Each tail is pulled
 *                      toward where the animation puts it in the model
 *                      (not below its simulated parent), then
 *                      skirt_iterations passes (int, 4) hold each bone's
 *                      length, keep each link between (1 - skirt_squeeze)
 *                      and (1 + skirt_stretch) times its animated length
 *                      (0.5 and 0.05), and keep every bone, every link and
 *                      the line across the middle of every panel out of
 *                      every shape, capsule against capsule. The shapes
 *                      are inside the garment: a segment found on a
 *                      shape's inner side is pushed out through its outer
 *                      side, so a leg never ends up outside the cloth. A
 *                      pushed tail keeps none of its velocity away from
 *                      the shape; max_angle holds as in chain mode. The
 *                      chains should hang from the same bone (the hips)
 *                      and have the same number of bones.
 * The model's Node3D must not be scaled non-uniformly. Debug command:
 * `ik <model path>` (bones, modifiers, last solve results, each spring
 * chain's tip offset, pose hash). */

/* The pose the modifiers of `model` (a Node3D) left this tick, or NULL when
 * it has none. Valid until the next fixed tick. */
const cw_pose *cw_skeleton_pose(cw_node model);
/* Running hash of every pose the model's modifiers flushed (0 when none). */
uint32_t cw_skeleton_hash(cw_node model);
/* World-space position of a bone in the model's current pose (zero when the
 * model has no pose or the bone is unknown). */
t3_vec3 cw_skeleton_bone_world(cw_node model, const char *bone);
/* Microseconds the modifier phase spent on the model's last tick (a
 * profiling figure from the host clock: never feed it into game state). */
float cw_skeleton_last_us(cw_node model);
/* Solve again, this tick, every model of `tree` (NULL: the current tree) whose
 * bones were written since this tick's modifier phase: an AnimationTree or
 * AnimationPlayer advanced by hand after the phase, bones posed by code.
 * Each starts from the bones as they are now (its new animated pose) and
 * its targets as they are now; foot placement and spring bones do not
 * advance a second time. Returns the models solved. The scene runner
 * (cw_scene_app) calls it after the app's fixed_update, so a MANUAL tree
 * advanced there gets its look-at, reaches and feet the same tick; an app
 * that runs cw_tree_fixed_update itself calls it after its own tick code.
 * Once per phase: a second call before the next phase does nothing. */
int cw_skeleton_modifiers_refresh(cw_tree *tree);

/* Per-joint limits for CCDIK3D: joint 0 is the link nearest the end bone
 * (r187 link order). Stored on the node (not in scene files). */
void cw_ccdik3d_set_joint(cw_node n, int joint, const cw_ccdik_link *limits);
/* Per-joint constraints for FABRIK3D: joint 0 is root_bone. */
void cw_fabrik3d_set_joint(cw_node n, int joint, const cw_ik_joint *j);
/* FootPlacement3D: per-leg results of the last tick (world-space ground
 * point and normal, the ankle's offset applied in metres, the animated
 * ankle's height above the model's ground plane, whether the ray hit).
 * false past the last leg. */
typedef struct cw_foot_state {
  t3_vec3 ground;
  t3_vec3 normal;
  float offset;
  float lift;
  bool hit;
} cw_foot_state;
/* One leg's results (see cw_foot_state); false past the last leg. */
bool cw_foot_placement_leg(cw_node n, int leg, cw_foot_state *out);
/* FootPlacement3D: the pelvis offset applied last tick (metres, <= 0) */
float cw_foot_placement_pelvis_offset(cw_node n);

#define CW_SPRING_MAX_JOINTS 32 /* longest SpringBoneChain3D (bones) */
/* SpringBone3D: its chains start again from the animated pose on the next
 * tick (a respawn, a cut to another scene); the same as setting `reset`. A
 * plain teleport needs no call: see teleport_distance. */
void cw_spring_bone3d_reset(cw_node n);
/* SpringBoneChain3D: world position of simulated joint `joint` (0: the
 * root bone's tail, the last: the tip) after the last tick; false past the
 * last joint or before the chain first ran. */
bool cw_spring_chain3d_joint_world(cw_node chain, int joint, t3_vec3 *out);
/* SpringBoneCollision3D: where the shape was last tick, in the world: a
 * capsule from *a to *b, a sphere at *a (== *b), and its radius (m). false
 * before it first ran or when its bone is unknown. */
bool cw_spring_collision3d_world(cw_node shape, t3_vec3 *a, t3_vec3 *b, float *radius);

#ifdef __cplusplus
}
#endif
#endif

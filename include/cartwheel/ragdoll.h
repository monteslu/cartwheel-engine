/* cartwheel ragdolls: a skeleton (cartwheel/ik.h cw_pose) turned into Box3D
 * bodies and joints, seeded from the current pose, and written back onto the
 * pose so the skinned mesh follows the bodies.
 *
 * A ragdoll is a list of parts. Each part is one bone (a capsule from the
 * bone's origin to a tip: a child bone's origin or an offset), hung from the
 * nearest ancestor bone that is also a part by a joint with limits:
 *   BALL   a cone around the bone's rest direction (relative to its parent
 *          part) and a twist range about the bone (shoulders, hips, neck,
 *          spine)
 *   HINGE  one axis (knees, elbows) with an angle range; 0 is the rest pose
 * Limits are measured from the REST pose (cw_pose rest_*), so a ragdoll
 * made from any animated pose keeps the same anatomy. Bones that are not
 * parts (fingers, face, twist bones) keep whatever local transform the pose
 * has, so they ride along with their part.
 *
 * Typical use (a character knocked down; see docs/guides/animation.md
 * "Ragdolls"):
 *   on the knockdown: r = cw_ragdoll_create(world, &def, &pose, &model_to_world);
 *                     cw_ragdoll_set_velocity(r, v, spin); cw_ragdoll_apply_impulse(...)
 *   each tick:        cw_world3d_step(world, dt);
 *                     cw_pose_capture(&pose);          the animated pose
 *                     cw_ragdoll_blend_pose(r, &pose, &model_to_world, w);  w = 1 while limp
 *                     cw_pose_flush(&pose);
 *   to get up:        when cw_ragdoll_resting(): pick a get-up clip by
 *                     cw_ragdoll_axis (face up or down), play it, and ramp
 *                     w from 1 to 0 over a few tenths of a second; then
 *                     cw_ragdoll_destroy(r).
 *
 * The ragdoll lives in the cw_world3d it was made in and collides with
 * whatever is there (ground, props). Its own parts never collide with each
 * other (one negative group per ragdoll unless def.group says otherwise).
 *
 * DETERMINISM. Box3D steps are bit-identical across hosts for the same
 * inputs, and everything here is + - * / sqrt on floats, so a ragdoll made
 * from the same pose and impulses lands the same way on every host
 * (cw_ragdoll_hash folds every body transform: a cross-host test compares
 * it). Keep ragdolls out of game rules anyway when the rules run on a fixed
 * tick of their own: treat them as presentation.
 *
 * COST: a part is one body, one capsule and one joint: a 14 part humanoid is
 * about the cost of 14 boxes in a stack while it moves; resting ragdolls
 * fall asleep (no cost). docs/guides/animation.md has measured numbers. */
#ifndef CW_RAGDOLL_H
#define CW_RAGDOLL_H

#include "ik.h"
#include "physics3d.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum { CW_RAGDOLL_BALL, CW_RAGDOLL_HINGE } cw_ragdoll_joint_kind;

typedef struct cw_ragdoll_part {
  const char *bone;            /* the bone this body drives */
  const char *tip;             /* the capsule ends at this bone's origin (usually a child); NULL: tip_offset */
  const char *attach;          /* hang from this part (a bone name) instead of the nearest part above it in the skeleton:
                                * for bones outside the chain they belong to (an IK rig's feet under the root); the bone's
                                * position is written as well as its rotation */
  t3_vec3 tip_offset;          /* model-space offset from the bone's origin at REST, when tip is NULL */
  float radius;                /* capsule radius in world metres */
  float density_scale;         /* times def.density; 0 = 1 */
  cw_ragdoll_joint_kind joint; /* how it hangs from its parent part (ignored for the root part) */
  float swing;                 /* BALL: cone half angle, radians (0: no cone limit) */
  float lower, upper;          /* BALL: twist about the bone; HINGE: angle about axis (radians, 0 = rest) */
  t3_vec3 axis;                /* HINGE: the hinge axis in model space at rest (e.g. (1,0,0) for a knee) */
} cw_ragdoll_part;

typedef struct cw_ragdoll_def {
  const cw_ragdoll_part *parts; /* parents before children is not required; a part's parent is found by bone */
  int count;                    /* up to CW_RAGDOLL_MAX_PARTS */
  float density;                /* kg/m^3 of the capsules: 1000 (water: a human's mass from body-sized capsules) */
  float friction;               /* contact friction: 0.7 */
  float restitution;            /* bounce: 0.05 */
  float joint_friction;         /* N*m resisting every joint's motion (a motor at zero speed): 2; 0 = limp */
  float linear_damping, angular_damping; /* 0.05, 0.6 */
  int layer;                    /* collision layer of every part: 0 */
  uint32_t mask;                /* layers the parts collide with: all */
  int group;                    /* 0: a fresh negative group per ragdoll (no self collision) */
} cw_ragdoll_def;
#define CW_RAGDOLL_MAX_PARTS 32

/* The defaults noted above, with parts/count from the arguments. */
cw_ragdoll_def cw_ragdoll_default_def(const cw_ragdoll_part *parts, int count);

typedef struct cw_ragdoll cw_ragdoll;

/* Bodies and joints for every part in `w`, placed where the pose's bones are
 * (its current model transforms: call cw_pose_capture or
 * cw_pose_update_model first) under `model_to_world` (rotation, translation
 * and a uniform scale). NULL (logged) when a part's bone or tip is missing
 * from the pose or the world is invalid. Velocities start at zero. */
cw_ragdoll *cw_ragdoll_create(cw_world3d w, const cw_ragdoll_def *def, const cw_pose *pose, const t3_mat4 *model_to_world);
/* Removes the bodies and joints and frees the ragdoll. NULL is fine. */
void cw_ragdoll_destroy(cw_ragdoll *r);

/* the number of parts (0 for NULL) */
int cw_ragdoll_part_count(const cw_ragdoll *r);
/* part index of a bone name, -1 when it is not a part */
int cw_ragdoll_find(const cw_ragdoll *r, const char *bone);
/* the part's physics body (cartwheel/physics3d.h), to read its velocity or
 * push it directly; a zero handle for a part out of range or a NULL ragdoll */
cw_body3d cw_ragdoll_body(const cw_ragdoll *r, int part);
/* the part's world transform (its bone's origin and rotation) */
cw_xform3 cw_ragdoll_part_transform(const cw_ragdoll *r, int part);
/* A model-space direction at REST (e.g. (0,0,1): the way the chest faced in
 * the bind pose) as it points now in world space, carried by the part.
 * Face up or face down: the chest's rest forward against world up. */
t3_vec3 cw_ragdoll_axis(const cw_ragdoll *r, int part, t3_vec3 rest_model_dir);

/* Every part moves with linear velocity v plus a spin `angular` (rad/s,
 * world) about the root part: the whole body carried by a blow. */
void cw_ragdoll_set_velocity(cw_ragdoll *r, t3_vec3 v, t3_vec3 angular);
/* An impulse (N*s) at a world point on one part; part < 0: the part whose
 * capsule is nearest the point. Returns the part hit. */
int cw_ragdoll_apply_impulse(cw_ragdoll *r, int part, t3_vec3 impulse, t3_vec3 point);
/* A force (N) at the root part's centre for this step (keep a ragdoll near a
 * point: a tether, wind). */
void cw_ragdoll_apply_force(cw_ragdoll *r, t3_vec3 force);

/* Write the bodies into the pose: every part bone gets the model rotation of
 * its body, the root part's bone also its position; other bones keep their
 * local transforms. Model transforms are updated; flush to show it. */
void cw_ragdoll_write_pose(const cw_ragdoll *r, cw_pose *pose, const t3_mat4 *model_to_world);
/* Blend the pose toward the ragdoll by w (0 = the pose as it is, 1 = the
 * ragdoll): local translations lerp, rotations slerp, per bone. Ramping w
 * down while a get-up clip plays hands the body from physics back to
 * animation without a pop. Call it every tick on the freshly captured pose
 * (w 0 included) until the ragdoll is destroyed: a part bone that no clip
 * writes would otherwise keep the last ragdoll angle, so a bone that still
 * holds exactly what the previous call left gets back the local transform
 * it had when the ragdoll was made. */
void cw_ragdoll_blend_pose(cw_ragdoll *r, cw_pose *pose, const t3_mat4 *model_to_world, float w);

/* true when every part moves slower than `speed` (m/s, and rad/s for spin) */
bool cw_ragdoll_resting(const cw_ragdoll *r, float speed);
/* the largest speed of any part (m/s) */
float cw_ragdoll_max_speed(const cw_ragdoll *r);
/* For a HINGE part: its current angle about the hinge (radians, 0 = rest);
 * for a BALL part: the angle between the bone and its rest direction
 * relative to its parent (the swing). 0 for the root. Probes check limits. */
float cw_ragdoll_joint_angle(const cw_ragdoll *r, int part);
/* FNV-1a over every body's position and rotation bits, folded into h */
uint32_t cw_ragdoll_hash(const cw_ragdoll *r, uint32_t h);

#ifdef __cplusplus
}
#endif
#endif

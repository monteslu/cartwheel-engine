/* cartwheel skeleton nodes: Skeleton3D (the bones of a skinned glTF model as
 * a node: names, parents, rest and pose transforms to read and set) and
 * BoneAttachment3D (a Node3D that rides a bone: a sword in a hand, a hat on a
 * head).
 *
 * WHAT A Skeleton3D IS. A view of a model's bones, not a second copy of them.
 * The bones stay the three.c objects glTF loaded (T3_BONE and the non-mesh
 * objects hanging off them, the set cartwheel/ik.h's cw_pose binds); bone
 * tracks, AnimationTree, skeleton modifiers and ragdolls keep writing those
 * objects exactly as before, and the Skeleton3D reads and writes the same
 * objects. Nothing changes for a scene that has no Skeleton3D.
 *
 * WHERE IT GOES. Either layout works:
 *   Hero (MeshInstance3D, mesh = a skinned glTF)      Skeleton (Skeleton3D)
 *     Skeleton (Skeleton3D)                             Hero (MeshInstance3D, mesh = ...)
 *       Hat (BoneAttachment3D, bone_name = "Head")      Hat (BoneAttachment3D)
 *       Reach (TwoBoneIK3D)                             Reach (TwoBoneIK3D)
 * The model is the Skeleton3D's own subtree when it holds bones (the right
 * column, or cw_node3d_set_object on the Skeleton3D itself), else its parent
 * Node3D (the left column: add a Skeleton3D to a model that already exists).
 * `model` (a node path from the Skeleton3D) names another Node3D instead.
 * Skeleton modifier nodes may be children of the Skeleton3D or, as before,
 * of the model's Node3D.
 *
 * BINDING. The skeleton binds when the model's bones exist (a glTF mesh
 * resource may load a few frames after the node enters the tree): at the
 * start of each fixed tick and before each render until it does, on any
 * query below made before that, and again when the model's objects are
 * replaced (another mesh, a hot reload). Bone
 * indices are stable while bound; `bone_count` is 0 and every query answers
 * "no bone" until then. Bones are ordered parents before children, depth
 * first, the same order as cw_pose_bind on the model.
 *
 * REST AND POSE. A bone's pose is its local translation, rotation and scale
 * now (the bone object's own fields: always current, whoever wrote them).
 * Its rest is the local transform it had when the skeleton bound: the glTF
 * file's node transforms when the Skeleton3D is in the scene before any
 * animation plays. Setting a pose writes the bone at once; an animation that
 * has a track on that bone overwrites it on its next tick (animate, then set,
 * or leave the bone untracked). `show_rest_only` holds every bone at rest
 * each tick, after animation and modifiers.
 *
 * SPACES. Local: relative to the bone's parent bone. Model: the model
 * Node3D's space (what cw_pose calls model space). Global: world space,
 * through the model node's current transform.
 *
 * OWNERSHIP. A bound Skeleton3D retains its bone objects (and nothing above
 * them), so a query never touches a freed bone; it lets go of them when it
 * unbinds or is freed. It holds no reference to a mesh or a three.c
 * skeleton, so it cannot keep a model alive.
 *
 * COST. Queries by index are O(1) (local) or O(depth of the bone) (model and
 * global). cw_skeleton3d_find_bone is O(bones). Each bound skeleton costs one
 * O(depth) check per tick and per frame; each BoneAttachment3D one bone
 * transform per tick and per frame.
 *
 * DETERMINISM. Poses are the bones' float bits; model transforms use + - * /
 * and sqrt only. cw_skeleton3d_hash folds every bone's local pose, so tests
 * compare skeletons across hosts.
 *
 * Debug command: `skeleton <node> [bones | bone <name> | set <name>
 * position|quaternion|scale <value> | rest [<name>] | hash]`.
 */
#ifndef CW_SKELETON_H
#define CW_SKELETON_H

#include "ik.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ── Skeleton3D ──────────────────────────────────────────────────────
 * Properties:
 *   model (string, "")          node path from the Skeleton3D to the Node3D
 *                               holding the model; "" picks it (see above)
 *   show_rest_only (bool)       hold the rest pose every tick
 *   bone_count (int, read-only) 0 until bound
 *   bound (bool, read-only)
 * Signals: skeleton_bound (the bones are there, or changed). */

/* Bones in the skeleton; 0 until bound (and for a node that is not a Skeleton3D). */
int cw_skeleton3d_bone_count(cw_node skeleton);
/* The Node3D whose space is model space (null handle until bound). */
cw_node cw_skeleton3d_model(cw_node skeleton);
/* Bone index by name, -1 when absent or not bound. O(bones). */
int cw_skeleton3d_find_bone(cw_node skeleton, const char *name);
/* The bone's name (the glTF node's; owned by three.c, valid while bound); NULL when out of range. */
const char *cw_skeleton3d_bone_name(cw_node skeleton, int bone);
/* Parent bone index; -1 for a root bone or when out of range. */
int cw_skeleton3d_bone_parent(cw_node skeleton, int bone);
/* The bone's three.c object (borrowed; valid while bound); NULL when out of range. */
t3_object *cw_skeleton3d_bone_object(cw_node skeleton, int bone);

/* A bone's local transform. */
typedef struct cw_bone_xform {
  t3_vec3 position;
  t3_quat rotation;
  t3_vec3 scale;
} cw_bone_xform;

/* The bone's local pose now. false (out untouched) when out of range. */
bool cw_skeleton3d_bone_pose(cw_node skeleton, int bone, cw_bone_xform *out);
/* Write a bone's local pose; a NULL part keeps what the bone has. Takes
 * effect at once (the next draw skins from it). */
void cw_skeleton3d_set_bone_pose(cw_node skeleton, int bone, const t3_vec3 *position, const t3_quat *rotation,
                                 const t3_vec3 *scale);
/* The bone's local rest transform. false when out of range. */
bool cw_skeleton3d_bone_rest(cw_node skeleton, int bone, cw_bone_xform *out);
/* Replace a bone's rest transform (the pose is not touched). */
void cw_skeleton3d_set_bone_rest(cw_node skeleton, int bone, const cw_bone_xform *rest);
/* Put one bone (bone >= 0) or every bone (bone < 0) back at rest. */
void cw_skeleton3d_reset_bone_pose(cw_node skeleton, int bone);
/* The bone in model space, from the bones' current fields. false when out of range. O(depth). */
bool cw_skeleton3d_bone_model_transform(cw_node skeleton, int bone, t3_mat4 *out);
/* The bone in world space: the model node's global transform times the above. */
bool cw_skeleton3d_bone_global_transform(cw_node skeleton, int bone, t3_mat4 *out);
/* The rest pose's model-space transform of a bone (every bone above it at rest too). */
bool cw_skeleton3d_bone_rest_model_transform(cw_node skeleton, int bone, t3_mat4 *out);

/* The whole skeleton as a cw_pose (cartwheel/ik.h), captured from the bones
 * now: for the solvers, ragdolls (cartwheel/ragdoll.h) and bulk reads. Owned
 * by the node; valid until the next call or until it unbinds. Change it and
 * call cw_pose_flush to write it back. NULL until bound. O(bones). */
cw_pose *cw_skeleton3d_pose(cw_node skeleton);
/* FNV-1a over every bone's local pose bits (0 until bound). O(bones). */
uint32_t cw_skeleton3d_hash(cw_node skeleton);
/* Bumped each time the skeleton binds (indices may have changed). */
uint32_t cw_skeleton3d_generation(cw_node skeleton);

/* ── BoneAttachment3D ────────────────────────────────────────────────
 * A Node3D that takes a bone's transform: its children (a sword, a hat, a
 * muzzle flash, a collision shape) ride the bone. Properties:
 *   bone_name (string)          the bone to follow
 *   skeleton (string, "")       node path to the Skeleton3D; "" = the nearest
 *                               Skeleton3D ancestor
 *   bone_idx (int, read-only)   -1 while the bone is not found
 * Its transform is written after animation, modifiers and physics each fixed
 * tick, and again just before each render from the bones as they are then,
 * so the attachment is drawn exactly where the bone is drawn, the same
 * frame, whoever moved the bone (a ragdoll written in the app's
 * fixed_update, an AnimationPlayer in UPDATE mode). Under the skeleton's
 * model it is placed relative to the model, so a model that is rendered
 * interpolated between ticks carries the attachment with it. Game code reads
 * its transform like any Node3D (cw_node3d_global_transform). Give an offset
 * with a child Node3D, not on the attachment itself (its own transform is
 * overwritten). */

/* The bone index the attachment follows, -1 when unresolved. */
int cw_bone_attachment3d_bone(cw_node attachment);
/* The Skeleton3D it follows (null handle when none). */
cw_node cw_bone_attachment3d_skeleton(cw_node attachment);

#ifdef __cplusplus
}
#endif
#endif

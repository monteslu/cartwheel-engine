/* cartwheel retargeting: play a clip made for one humanoid skeleton on
 * another with different proportions, rest pose and bone names.
 *
 * HOW. Both skeletons are described by a bone map: which bone fills each
 * slot of a standard humanoid (hips, spine, chest, neck, head, shoulders,
 * arms, hands, legs, feet, toes, fingers). cw_bone_map_auto fills a map from
 * the bone names of the common rigs; a few lines of text override it.
 * For every frame:
 *   1. Rotations go across relative to the rest poses, in model space: a
 *      target bone turns from its rest orientation by the turn the source
 *      bone made from its own. Where the rest poses differ (a T-pose and an
 *      A-pose), the spine, arms and fingers are first lined up by direction
 *      (align_rest), so an arm that points down in the source points down in
 *      the target. Legs turn from their own rest pose unless align_legs asks.
 *   2. The hips move by the source's motion from rest times the ratio of the
 *      leg lengths, so a short character takes short steps.
 *   3. foot_ik: each foot is put where the source's foot is, scaled by the
 *      same ratio (its height above the ground and its travel forward and
 *      sideways from where it rests, so the target keeps its own stance
 *      width), with the leg solved as two-bone IK (the knee keeps its plane)
 *      and the foot keeping its transferred rotation. A leg that cannot
 *      reach lowers the hips until it can, by at most half a leg; a stride
 *      longer than the target's legs is then left short.
 *      A planted source foot is a planted target foot, whatever the thigh
 *      and shin proportions. hand_ik does the same for the hands, relative
 *      to the chest and scaled by arm length (two hands on a prop).
 * Bones no slot names keep their rest pose; source bones between mapped ones
 * (twist bones, extra spine joints) are carried by the model-space transfer.
 *
 * WHERE. At import: a model's .cwimport sidecar names a source model and
 * cwpack bakes the retargeted clips into the packed model (docs/guides/
 * animation.md "Retargeting"), so the game loads them like any glTF clip.
 * At load from C: cw_retarget_clip turns a cw_anim into a new one for the
 * target. Per frame: cw_retarget_apply on two poses (a live puppet).
 *
 * DETERMINISM. + - * / sqrt and cartwheel/math.h only, fixed iteration
 * counts: the same input gives the same bits on every host. Nothing here
 * needs a scene tree: poses are cw_pose (cartwheel/ik.h), bound to a glTF
 * (cw_pose_bind, or a Skeleton3D's pose) or free-standing. */
#ifndef CW_RETARGET_H
#define CW_RETARGET_H

#include "anim.h"
#include "ik.h"

#ifdef __cplusplus
extern "C" {
#endif

/* The humanoid slots. L/R are the character's own left and right. */
typedef enum cw_humanoid_bone {
  CW_HB_HIPS, CW_HB_SPINE, CW_HB_CHEST, CW_HB_UPPER_CHEST, CW_HB_NECK, CW_HB_HEAD,
  CW_HB_LEFT_SHOULDER, CW_HB_LEFT_UPPER_ARM, CW_HB_LEFT_LOWER_ARM, CW_HB_LEFT_HAND,
  CW_HB_RIGHT_SHOULDER, CW_HB_RIGHT_UPPER_ARM, CW_HB_RIGHT_LOWER_ARM, CW_HB_RIGHT_HAND,
  CW_HB_LEFT_UPPER_LEG, CW_HB_LEFT_LOWER_LEG, CW_HB_LEFT_FOOT, CW_HB_LEFT_TOES,
  CW_HB_RIGHT_UPPER_LEG, CW_HB_RIGHT_LOWER_LEG, CW_HB_RIGHT_FOOT, CW_HB_RIGHT_TOES,
  /* fingers: thumb, index, middle, ring, little; three joints each, from the palm out */
  CW_HB_LEFT_THUMB_1, CW_HB_LEFT_THUMB_2, CW_HB_LEFT_THUMB_3,
  CW_HB_LEFT_INDEX_1, CW_HB_LEFT_INDEX_2, CW_HB_LEFT_INDEX_3,
  CW_HB_LEFT_MIDDLE_1, CW_HB_LEFT_MIDDLE_2, CW_HB_LEFT_MIDDLE_3,
  CW_HB_LEFT_RING_1, CW_HB_LEFT_RING_2, CW_HB_LEFT_RING_3,
  CW_HB_LEFT_LITTLE_1, CW_HB_LEFT_LITTLE_2, CW_HB_LEFT_LITTLE_3,
  CW_HB_RIGHT_THUMB_1, CW_HB_RIGHT_THUMB_2, CW_HB_RIGHT_THUMB_3,
  CW_HB_RIGHT_INDEX_1, CW_HB_RIGHT_INDEX_2, CW_HB_RIGHT_INDEX_3,
  CW_HB_RIGHT_MIDDLE_1, CW_HB_RIGHT_MIDDLE_2, CW_HB_RIGHT_MIDDLE_3,
  CW_HB_RIGHT_RING_1, CW_HB_RIGHT_RING_2, CW_HB_RIGHT_RING_3,
  CW_HB_RIGHT_LITTLE_1, CW_HB_RIGHT_LITTLE_2, CW_HB_RIGHT_LITTLE_3,
  CW_HB_COUNT
} cw_humanoid_bone;

/* A slot's name as map text writes it: "hips", "left_upper_arm", "right_index_2"; NULL out of range. */
const char *cw_humanoid_bone_name(cw_humanoid_bone slot);
/* The slot of that name, -1 when there is none. */
int cw_humanoid_bone_find(const char *name);

/* Which bone of one skeleton fills each slot: a pose bone index, -1 for none. */
typedef struct cw_bone_map {
  int bone[CW_HB_COUNT];
} cw_bone_map;

/* Fill a map from the pose's bone names. Understood: Mixamo ("mixamorig:LeftUpLeg"), VRM and glTF
 * humanoid names ("leftUpperLeg", "J_Bip_L_UpperLeg"), Blender Rigify and Unreal style ("thigh.L",
 * "thigh_l", "upperarm_r", "spine_01"), 3ds Max Biped ("Bip001 L Thigh") and plain names with a side
 * ("UpperLegL", "Upper_Leg.L", "LeftFoot"). Spine bones numbered in a row fill spine, chest and
 * upper_chest in order. Returns the number of slots filled; slots it cannot place stay -1. */
int cw_bone_map_auto(cw_bone_map *m, const cw_pose *p);
/* Change a map from text, one `slot = BoneName` per line (`#` comments, blank lines; an empty name
 * clears the slot): the override for what cw_bone_map_auto gets wrong or cannot know.
 *   left_hand = GripL
 *   left_toes =
 * false (logged with the line) on a slot or bone that does not exist; the lines before it stay applied. */
bool cw_bone_map_apply_text(cw_bone_map *m, const cw_pose *p, const char *text);
/* One slot by name; bone NULL or "" clears it. false (logged) on an unknown slot or bone. */
bool cw_bone_map_set(cw_bone_map *m, const cw_pose *p, const char *slot, const char *bone);
/* The map as that text, every filled slot (cw_alloc'd). */
char *cw_bone_map_to_text(const cw_bone_map *m, const cw_pose *p);
/* Can it be retargeted: hips and both legs to the feet are there. When not, `missing` (if given) gets the
 * names of the slots that are not, separated by spaces. */
bool cw_bone_map_usable(const cw_bone_map *m, char *missing, int cap);

typedef struct cw_retarget_opts {
  bool align_rest;   /* line the spine, arms and fingers up by direction where the rest poses differ (default true) */
  bool align_legs;   /* the legs too (default false: each leg turns from its own rest, so a source that stands
                      * with its legs apart or bent does not bow the target's knees; the feet are placed by IK) */
  bool foot_ik;      /* put each foot where the source's foot is (default true) */
  bool hand_ik;      /* put each hand where the source's is, relative to the chest (default false) */
  float hips_scale;  /* hips travel per unit of the source's; <= 0: the ratio of the leg lengths (default) */
  float fps;         /* cw_retarget_clip: keys per second in the new clip; <= 0: 30 */
  bool plant_feet;   /* cw_retarget_clip: take the slide out of planted feet (cw_foot_plant), for walks and runs:
                      * a foot on the ground travels at one steady speed however uneven the source's is (default false) */
  bool cyclic;       /* plant_feet: the clip repeats (its last frame is its first), so a stance may span its end */
} cw_retarget_opts;
#define CW_RETARGET_OPTS_DEFAULT ((cw_retarget_opts){ .align_rest = true, .foot_ik = true, .hand_ik = false, .hips_scale = 0, .fps = 30 })

/* Planted feet. Given where the two feet are in every frame of a clip (model space; left[i] and right[i] at
 * times[i], count frames, the clip length seconds long), a foot is planted while it is within height (a
 * share of its own lowest to highest travel) of its lowest point. Returns the mean velocity of planted feet
 * over the ground (x and z, units per second): a character moved at the opposite of it walks without
 * sliding, as far as the clip allows. With adjust, the positions are rewritten so that it does not slide
 * at all: every planted frame moves onto a straight line at that velocity through its stance, and the
 * frames around a stance ease in and out over blend seconds. Heights are left alone. Deterministic. */
typedef struct cw_foot_plant_opts {
  float height;  /* 0..1 (default 0.1) */
  float blend;   /* seconds (default 0.1) */
  bool adjust;   /* rewrite the positions (default true) */
  bool cyclic;   /* the last frame repeats the first: a stance may span the end of the clip */
} cw_foot_plant_opts;
#define CW_FOOT_PLANT_OPTS_DEFAULT ((cw_foot_plant_opts){ .height = 0.1f, .blend = 0.1f, .adjust = true, .cyclic = false })
/* The planted-feet pass described above; opts NULL: the defaults. Returns the planted feet's mean velocity. */
t3_vec3 cw_foot_plant(t3_vec3 *left, t3_vec3 *right, const float *times, int count, float length, const cw_foot_plant_opts *opts);

/* A prepared pairing of two skeletons. Made from their rest poses (the poses' rest_* arrays; cw_pose_bind
 * captures them, cw_pose_set_rest sets them) and their maps. It keeps no pointer to either pose. */
typedef struct cw_retarget cw_retarget;
/* NULL (logged, naming the missing slots) when either map is not usable. opts NULL: the defaults. */
cw_retarget *cw_retarget_new(const cw_pose *src, const cw_bone_map *src_map, const cw_pose *dst,
                             const cw_bone_map *dst_map, const cw_retarget_opts *opts);
void cw_retarget_free(cw_retarget *r); /* NULL is fine */
/* The source's current pose (its model transforms up to date: cw_pose_capture or cw_pose_update_model)
 * onto dst: writes dst's local transforms and model transforms. The poses must be the ones the pairing
 * was made from (same bone order). Does not touch the bones: cw_pose_flush(dst) does. */
void cw_retarget_apply(const cw_retarget *r, const cw_pose *src, cw_pose *dst);
/* Where the pairing puts a foot for the source's current pose (side 0 left, 1 right), in the target's model space. */
t3_vec3 cw_retarget_foot_target(const cw_retarget *r, const cw_pose *src, int side);
/* cw_retarget_apply with the two foot positions given (left, right; model space) in place of the source's:
 * what a foot planting pass, or a game that puts a foot on a ledge, hands in. NULL feet: cw_retarget_apply. */
void cw_retarget_apply_feet(const cw_retarget *r, const cw_pose *src, cw_pose *dst, const t3_vec3 *feet);
float cw_retarget_scale(const cw_retarget *r); /* the hips and foot scale in use (target leg length over source) */
/* How far the last cw_retarget_apply left a foot from where it was asked to be, in model units (0 with
 * foot_ik and the target in reach; the proportion error without it). side 0 left, 1 right. */
float cw_retarget_foot_error(const cw_retarget *r, int side);

/* A new clip for the target skeleton from one made for the source: `clip` is sampled at opts.fps, each
 * frame goes through cw_retarget_apply, and the result has a quaternion track per mapped target bone and
 * a position track for the hips, at "<dst_model_path>@<bone>:<prop>" (dst_model_path as in
 * cw_anim_from_t3_clip). The clip's bone tracks are matched to source bones by bone name, whatever
 * their model path. Length, loop mode, name and event keys carry over; other tracks do not. Both poses
 * are left in their rest pose. The caller holds the new clip's reference; null (logged) on an error. */
cw_anim cw_retarget_clip(cw_anim clip, cw_pose *src, const cw_bone_map *src_map, cw_pose *dst,
                         const cw_bone_map *dst_map, const char *dst_model_path, const cw_retarget_opts *opts);
/* The velocity of a clip's planted feet over the ground, in the pose's model units per second (x and z):
 * move a character at the opposite of it (times its node scale) and its feet stay where they land. The clip's
 * bone tracks are matched to the pose's bones by name; the pose is left in its rest pose. cyclic as above. */
t3_vec3 cw_anim_ground_velocity(cw_anim clip, cw_pose *pose, int left_foot_bone, int right_foot_bone, bool cyclic);

#ifdef __cplusplus
}
#endif
#endif

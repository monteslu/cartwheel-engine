/* cartwheel/director.h: a cinematic camera. Shots are named by type and subjects (push in on the boss, a hero angle
 * on the player, an insert on a hand, a reaction close-up, a whip pan, a top-down) and solved against those subjects
 * every frame, so the framing holds while people move: a close-up never ends up on the back of a head, a shoulder,
 * a hand or somebody walking through the lens.
 *
 *   cw_director *d = cw_director_new(&(cw_director_desc){ .aspect = 16.f / 9, .ray = ray, .user = world });
 *   cw_director_set_subject(d, 0, &(cw_dir_subject){ .eye = head, .chest = chest, .feet = feet, .face = fwd, ... });
 *   cw_director_cut(d, &(cw_shot){ .type = CW_SHOT_PUSH, .subj = { 0 }, .nsubj = 1, .dur = 3 });
 *   cw_director_update(d, dt, &cam);        // every frame: cam.pos, cam.look, cam.fov for the Camera3D
 *   cw_director_release(d, 0.6f);           // ease back to the game's camera (cw_director_set_gameplay)
 *
 * Subjects are the game's: every frame give each one its eye point (between the eyes), chest, feet, the way the face
 * points (level), the hands and a radius (shoulder half-width). Anything else that may get in the way is a subject
 * too (a passer-by, a foe): the solver keeps every subject's head and body out of the line to the face it frames.
 * The world is the game's: `ray` answers whether a segment hits something solid (walls, pillars, terrain); without
 * it only people are avoided.
 *
 * Solving: each shot type has a set of candidate placements around its subjects (angle, distance, height); each is
 * scored: the key point (the face, or the hand of an insert) in clear view (rays to the eyes and chest), the face
 * turned toward the lens (a close-up needs the face, never a profile into hair or the back of the head), no other
 * subject's head, hair or body across the line to the face, the subject's own hands off the face, nothing solid in
 * front of the lens, the key point inside the frame and not too close, the 180-degree line of a two-shot kept from
 * the last cut, and variety against the last shot. The best wins. While the shot runs it is scored again every
 * 0.1 s against where everyone is now; when it fails (someone walks into the lens, the subject turns away), it solves
 * again near where the lens is and eases there, or cuts when the move would be long or pass through someone. A close
 * shot with no clean placement steps back to a medium close-up, then a wide, so the frame always has the subject.
 *
 * The camera never shakes: no handheld noise, no shake; motion is eased (a critically damped follow, smooth pushes,
 * a whip pan's quick ease in and out). Impacts belong to the pad's rumble (cartwheel/input.h cw_input_rumble).
 *
 * Shot types (cartwheel/story.h cw_shot_type): WIDE (everyone), ESTABLISH (high and far), TWO (two people, side on),
 * OTS (over subj[0]'s shoulder onto subj[1]'s face), MCU (head and shoulders), CU (the face), PUSH (a slow dolly in
 * on subj[0] over dur), REACT (a close-up on subj[0] listening to subj[1]), INSERT (subj[0]'s hand or head, or the
 * point), HERO (low, looking up at subj[0]), WHIP (a fast pan from the current framing onto subj[0], landing as an
 * MCU or a two-shot), TOP (straight down over everyone), RELEASE (cw_director_release).
 *
 * Debug fields (cw_director_publish(d, "dir")): dir.active, dir.type (the shot asked), dir.solved (the type played
 * after any step back), dir.score, dir.resolves (live re-solves), dir.cuts, dir.blocked_frames (frames in which
 * another subject's head or body crossed the line to the key point within dir_block_radius, or a ray hit: must stay
 * 0 for a clean shot), dir.away_frames (close-up frames whose face pointed away from the lens), dir.out_frames (key
 * point outside the frame), dir.reversals (frames where the
 * camera's motion turned back on itself along an axis outside a cut, a whip or the ease back to the game's camera: a shake shows here, a smooth move
 * never does), dir.key_x, dir.key_y (the key point on screen, -1..1). */
#ifndef CW_DIRECTOR_H
#define CW_DIRECTOR_H

#include "base.h"
#include "story.h"
#include "three.h"

#ifdef __cplusplus
extern "C" {
#endif

#define CW_DIR_MAX_SUBJECTS 16

typedef struct cw_dir_subject {
  t3_vec3 eye;        /* between the eyes */
  t3_vec3 chest;      /* middle of the chest */
  t3_vec3 feet;       /* on the ground under them */
  t3_vec3 face;       /* the way the face points, level (unit; 0: from eye minus chest) */
  t3_vec3 hand_l, hand_r; /* 0,0,0: at the chest */
  float radius;       /* shoulder half-width (0: 0.25 m) */
  float head_radius;  /* head with hair (0: 0.14 m) */
  bool visible;       /* hidden subjects are neither framed nor in the way */
} cw_dir_subject;

typedef struct cw_shot {
  cw_shot_type type;
  int subj[3];        /* subject indices */
  int nsubj;
  float dur;          /* PUSH: seconds of the dolly (0: 4); any type: how long it holds before cw_director_shot_done */
  float dist, height, fov; /* 0: the type's own */
  cw_shot_part part;  /* INSERT */
  t3_vec3 point;      /* INSERT with CW_SHOT_PART_POINT */
} cw_shot;

typedef struct cw_dir_camera { t3_vec3 pos, look; float fov; } cw_dir_camera; /* fov: vertical, degrees */

typedef struct cw_director_desc {
  float aspect;       /* the view's width / height (0: 16/9) */
  /* a solid thing on the segment from a to b: true and *t (0..1) of the first hit. NULL: no world */
  bool (*ray)(t3_vec3 a, t3_vec3 b, float *t, void *user);
  void *user;
  float ease;         /* the follow's rate (0: 5; higher follows tighter) */
  bool no_solve;      /* the first candidate of each type, never scored or re-solved (a gate's control only) */
  float test_noise;   /* metres of handheld-style noise added to the output (a gate's control only: it must show as shake) */
} cw_director_desc;

typedef struct cw_director cw_director;
cw_director *cw_director_new(const cw_director_desc *desc); /* a director (desc may be NULL: defaults); free with cw_director_free */
void cw_director_free(cw_director *d); /* frees it (NULL is a no-op) */
void cw_director_set_subject(cw_director *d, int index, const cw_dir_subject *s); /* index < CW_DIR_MAX_SUBJECTS */
void cw_director_set_gameplay(cw_director *d, const cw_dir_camera *cam); /* the game's camera, for ease in and out */
void cw_director_cut(cw_director *d, const cw_shot *shot); /* cut to a shot now (a WHIP pans from the current framing; RELEASE eases out) */
void cw_director_release(cw_director *d, float seconds); /* ease to the gameplay camera, then inactive */
void cw_director_stop(cw_director *d);                   /* inactive at once */
bool cw_director_active(const cw_director *d); /* a shot or a release is playing */
bool cw_director_shot_done(const cw_director *d);        /* the shot has run its dur */
float cw_director_shot_time(const cw_director *d); /* seconds since the shot was cut */
/* every frame (render-side or fixed tick; deterministic for the same inputs); false when inactive (cam untouched) */
bool cw_director_update(cw_director *d, float dt, cw_dir_camera *cam);
float cw_director_whip(const cw_director *d);            /* 0..1 how strongly a whip pans now */
cw_shot_type cw_director_solved_type(const cw_director *d); /* the type playing after any step back (a close-up with no clean placement plays as an MCU or WIDE) */
void cw_director_publish(cw_director *d, const char *prefix); /* debug fields under prefix (dir.*: see the top of this header) */
/* the measures (the debug fields' values), for a game's own checks; cumulative since cw_director_new */
typedef enum { CW_DIR_M_BLOCKED, CW_DIR_M_AWAY, CW_DIR_M_OUT, CW_DIR_M_REVERSALS, CW_DIR_M_RESOLVES, CW_DIR_M_CUTS, CW_DIR_M_KEY_X, CW_DIR_M_KEY_Y } cw_dir_measure;
float cw_director_measure(const cw_director *d, cw_dir_measure m); /* one measure's value */
/* the story's shot beats as shots: subjects map cast index -> subject index (NULL: the same index) */
void cw_director_cut_story(cw_director *d, const cw_story_shot *s, const int *cast_to_subject);

#ifdef __cplusplus
}
#endif
#endif

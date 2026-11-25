/* cartwheel/story.h: dialogue and story. Scenes of spoken lines, choices on a pad, flags and stats that change what
 * happens next, barks and banter that play over gameplay, subtitles, skip, save points.
 *
 * A story is written as a text script and compiled ahead of time (tools/story-compile.mjs) into a .cwstory asset
 * (or a C array linked into the game: --c). The guide (docs/guides/story-voice-director.md) has the script format:
 *
 *   cast keeper "The Keeper" voice=bm_george color=#ffd27a
 *   stat trust = 0
 *   flag gave_name
 *   scene gate save
 *     shot two keeper traveller
 *     say keeper anger:0.6 "Who are you?"
 *     choice "Answer the keeper" timeout=12 default=1
 *       option "A courier."
 *         add trust 2
 *         set gave_name
 *       option "Nobody."
 *     end
 *     if flag gave_name and stat trust >= 2
 *       emit open_gate 1
 *     end
 *   end
 *   pool keeper.idle cooldown=4
 *     line keeper "Mind the third plank."
 *   end
 *
 * In the game:
 *
 *   cw_story *st = cw_story_new(&(cw_story_desc){ .path = "main.cwstory", .speech = sp, .on_shot = shot, .on_emit = ev });
 *   cw_story_play(st, "gate");                   // a scene starts (gameplay may keep running under it, or wait)
 *   cw_story_tick(st, dt);                       // in fixed_update: input, timing, the next beat
 *   cw_story_bark(st, "keeper.idle");            // a line from a pool, over gameplay (never blocks)
 *   cw_story_draw(st, font, (cw_rect){ 0 });        // in render, inside cw_r2d_begin / end: subtitles and choices
 *
 * Play is pad only: advance (ui_accept by default) ends the line now, holding skip (ui_back by default) for
 * CW_STORY_SKIP_HOLD seconds skips the rest of the scene, up and down move the choice cursor and advance picks.
 * Lines also end on their own: when the voice ends (the speech pack's length) plus a short pause, or after a reading
 * time from the text's length when there is no voice. A choice with timeout= picks its default= when time runs out.
 * Skipping never loses a choice: a skipped scene takes each choice's default and runs every flag, stat, emit and shot
 * beat on the way, so the story's state is the same as if it had been played through by a player who chose defaults.
 *
 * Timing is in fixed ticks (cw_story_tick's dt) and voice lengths come from the pack, never from the mixer: the
 * same inputs give the same story on every host and with audio off, so gates can compare it.
 *
 * Pools (barks, banter, reactive lines): cw_story_bark picks one line of the pool whose condition holds, never the
 * line the pool said last, preferring the one said longest ago; a pool's cooldown= holds it after it speaks; a pool
 * marked once says each line at most once in the whole story (saved). A bark plays as an aside: a small subtitle and
 * the voice, with no camera cut and no input, over gameplay or a scene's waiting. A bark of a lower priority than the
 * aside playing waits its cooldown out; a higher one replaces it. An aside never interrupts a scene's line: it is
 * dropped (and reported in story.asides_dropped).
 *
 * Saves: cw_story_save writes the flags, stats, once-pool lines said and the last save point reached into a buffer
 * (named entries: renaming a flag in the script loses its value; adding or reordering never does); cw_story_load
 * puts them back, and cw_story_resume_scene names the scene to play to resume. A save point is a scene marked save,
 * or a save beat inside one.
 *
 * Debug fields (cw_story_publish(st, "story")): story.scene (the playing scene's index, -1 none), story.beat,
 * story.line_key (the line on screen now, 0 none), story.lines (lines said), story.asides, story.asides_dropped,
 * story.choices (choices made), story.choice (the last option picked, from 0), story.choice_cursor, story.skips,
 * story.scene_skips, story.repeats (a line from a pool said twice running: must stay 0), story.waiting (1 when a
 * choice waits), story.emits, story.shots, story.saves, story.flags_hash (FNV of every flag and stat). */
#ifndef CW_STORY_H
#define CW_STORY_H

#include "base.h"
#include "render2d.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct cw_story cw_story;
typedef struct cw_speech cw_speech; /* cartwheel/speech.h */

/* a line's feeling: drives the speaker's face (cw_face_set_emotion) */
typedef enum { CW_EMO_CALM, CW_EMO_JOY, CW_EMO_ANGER, CW_EMO_SORROW, CW_EMO_FEAR, CW_EMO_SURPRISE, CW_EMO_COUNT } cw_emotion;

/* shot types a script's shot beat asks for (the director's, cartwheel/director.h, in the same order) */
typedef enum {
  CW_SHOT_WIDE, CW_SHOT_ESTABLISH, CW_SHOT_TWO, CW_SHOT_OTS, CW_SHOT_MCU, CW_SHOT_CU,
  CW_SHOT_PUSH, CW_SHOT_REACT, CW_SHOT_INSERT, CW_SHOT_HERO, CW_SHOT_WHIP, CW_SHOT_TOP,
  CW_SHOT_RELEASE, /* back to the game's own camera */
  CW_SHOT_TYPES
} cw_shot_type;
typedef enum { CW_SHOT_PART_HEAD, CW_SHOT_PART_HAND_L, CW_SHOT_PART_HAND_R, CW_SHOT_PART_POINT, CW_SHOT_PART_CHEST } cw_shot_part;

typedef struct cw_story_line {
  int who;              /* cast index */
  const char *speaker;  /* the cast member's display name */
  const char *cast_id;  /* the cast member's id in the script */
  const char *text;
  uint32_t key;         /* the voice pack's key (FNV-1a of "cast_id|text") */
  cw_emotion emotion;
  float amount;         /* 0..1 */
  int to;               /* who it is said to, -1 none */
  int shot;             /* a cw_shot_type asked with shot=, -1 none */
  uint32_t color;       /* 0xRRGGBB, the speaker's name colour */
  float seconds;        /* how long it plays (voice length, or a reading time) */
  bool aside;           /* a bark or an aside beat: over gameplay */
} cw_story_line;

typedef struct cw_story_shot {
  cw_shot_type type;
  int subj[3];          /* cast indices, -1 unused */
  int nsubj;
  float dur, dist, fov, height; /* 0: the director's own */
  cw_shot_part part;
} cw_story_shot;

typedef struct cw_story_desc {
  const char *path;          /* the .cwstory asset, or: */
  const void *data;          /* the compiled bytes (story-compile.mjs --c); copied */
  size_t size;
  cw_speech *speech;         /* optional: lines play their voice and take its length */
  int player;                /* whose actions drive it (0 = player 1) */
  bool any_player;           /* every player's pad drives it */
  const char *advance_action, *skip_action, *up_action, *down_action; /* NULL: ui_accept, ui_back, ui_up, ui_down */
  float reading_cps;         /* characters a second for a line with no voice (0: 14) */
  float line_gap;            /* seconds after a voice ends before the next beat (0: 0.45) */
  bool wait_for_advance;     /* a scene's line waits for advance after it ends (visual novel pacing); default off */
  void (*on_line)(const cw_story_line *line, void *user);   /* a line starts (say, aside or bark) */
  void (*on_shot)(const cw_story_shot *shot, void *user);   /* a shot beat, or a say with shot= */
  void (*on_emit)(const char *name, int arg, void *user);   /* an emit beat: the game's events */
  void (*on_scene_end)(const char *scene, bool skipped, void *user);
  void *user;
} cw_story_desc;

#define CW_STORY_SKIP_HOLD 0.8f    /* seconds skip is held to skip a scene */
#define CW_STORY_MAX_OPTIONS 6

cw_story *cw_story_new(const cw_story_desc *desc); /* NULL (logged) when the data is missing or malformed */
void cw_story_free(cw_story *st); /* frees the story (NULL is a no-op) */

/* scenes */
bool cw_story_play(cw_story *st, const char *scene);  /* false (logged) when there is no such scene */
bool cw_story_active(const cw_story *st);             /* a scene is playing */
const char *cw_story_scene(const cw_story *st);       /* the playing scene's name, NULL none */
int cw_story_scene_index(const cw_story *st, const char *scene); /* -1 none */
bool cw_story_seen(const cw_story *st, const char *scene); /* the scene has ended (played or skipped) */
void cw_story_stop(cw_story *st);                     /* the scene stops where it is (no skip: later beats do not run) */
void cw_story_tick(cw_story *st, float dt);           /* in fixed_update, every tick */

/* what is on screen now */
const cw_story_line *cw_story_current(const cw_story *st); /* the scene's line, NULL none */
const cw_story_line *cw_story_aside(const cw_story *st);   /* the aside or bark playing, NULL none */
float cw_story_line_time(const cw_story *st, bool aside);  /* seconds into that line */
bool cw_story_choosing(const cw_story *st);                /* a choice waits */
int cw_story_options(const cw_story *st, const char **text, bool *enabled, int cap); /* the choice's options */
int cw_story_cursor(const cw_story *st); /* the option under the cursor, from 0 */
const char *cw_story_prompt(const cw_story *st); /* the waiting choice's prompt, NULL when none waits */

/* input from code (bots, tests); the pad does the same */
void cw_story_advance(cw_story *st);
void cw_story_choose(cw_story *st, int option);  /* an enabled option of the waiting choice */
void cw_story_skip_scene(cw_story *st); /* skip the rest of the scene (as a held skip does) */

/* pools */
const cw_story_line *cw_story_bark(cw_story *st, const char *pool); /* NULL: nothing eligible, cooling down or busy */
bool cw_story_pool_ready(const cw_story *st, const char *pool); /* the pool exists and is not cooling down */

/* flags and stats (by the script's names; an unknown name is logged once and reads 0) */
bool cw_story_flag(const cw_story *st, const char *flag);
void cw_story_set_flag(cw_story *st, const char *flag, bool on); /* set a flag by name */
int cw_story_stat(const cw_story *st, const char *stat); /* a stat's value by name */
void cw_story_set_stat(cw_story *st, const char *stat, int value); /* set a stat by name */
void cw_story_add_stat(cw_story *st, const char *stat, int delta); /* add to a stat by name */

/* cast */
int cw_story_cast_count(const cw_story *st);
int cw_story_cast_find(const cw_story *st, const char *id); /* -1 none */
const char *cw_story_cast_id(const cw_story *st, int who); /* a cast member's id in the script, NULL out of range */
const char *cw_story_cast_name(const cw_story *st, int who); /* a cast member's display name, NULL out of range */

/* saves */
size_t cw_story_save(const cw_story *st, void *buf, size_t cap); /* bytes written (or needed, when cap is short) */
bool cw_story_load(cw_story *st, const void *buf, size_t size); /* puts a saved story back (cw_story_reset first); false when the buffer is not a story save */
const char *cw_story_resume_scene(const cw_story *st);           /* the last save point's scene, NULL none */
void cw_story_reset(cw_story *st);                               /* a new game: every flag, stat and pool as declared */

/* subtitles: the line on screen as a box near the bottom of `area` (canvas units; w or h 0: the whole canvas), the speaker's name over it, at the readable
 * size floor (2.4% of the canvas height, scaled by the player's Text size setting), the choice menu above it with
 * the cursor, and an aside as a smaller line at the top. A skip hint shows while skip is held. */
void cw_story_draw(cw_story *st, cw_font font, cw_rect area);
void cw_story_set_subtitles(cw_story *st, bool on); /* off: scene lines and asides are not drawn (choices still are) */
bool cw_story_subtitles(const cw_story *st); /* subtitles are drawn */

/* the speaking face: who speaks now (scene line first, else the aside), the line's key, seconds into it and its
 * emotion. false when nobody speaks. With cartwheel/speech.h: cw_speech_mouth(sp, key, t, &open, &shape). */
bool cw_story_speaking(const cw_story *st, int *who, uint32_t *key, float *t, cw_emotion *emotion, float *amount);

void cw_story_publish(cw_story *st, const char *prefix); /* debug fields under prefix (one story at a time) */

#ifdef __cplusplus
}
#endif
#endif

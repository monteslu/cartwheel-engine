/* cartwheel/speech.h: voiced lines from a pre-generated speech pack, and faces that speak them.
 *
 * Lines are voiced ahead of time by tools/speech-bake.mjs (Kokoro-82M run from node, on the CPU) into one .cwspeech
 * asset per game: each line's audio (IMA ADPCM, mono) and its mouth envelope (how open, what shape, 100 times a
 * second), filed under the line's key (FNV-1a of "cast_id|text": cartwheel/story.h computes the same, so a story's
 * lines find their voices with no table in between).
 *
 *   cw_speech *sp = cw_speech_load("main.cwspeech");
 *   cw_voice v = cw_speech_play(sp, key, NULL);      // on the speech bus (its own volume, under the master)
 *   cw_speech_mouth(sp, key, t, &open, &shape);       // the lips at t seconds into the line
 *
 * A story (cartwheel/story.h) given the pack plays each line's voice itself and times the line by it.
 *
 * Faces: cw_face drives a glTF character's morph targets (cartwheel/morph.h) from a mouth (open, shape) and an
 * emotion, eased the way a face moves, with blinks. Channels are found by name on the model: the defaults below
 * cover the common names (ARKit style jawOpen / mouthSmile_L, VRM aa / oh / happy / angry, and plain smile, frown,
 * browUp, blink); cw_face_map names a model's own. A channel the model lacks is left out (cw_face_channels says which
 * were found). The weights are computed from the story's ticks, not the mixer, so a face is the same on every host
 * with or without sound.
 *
 *   cw_face *f = cw_face_new(model_root, NULL);
 *   cw_face_set_emotion(f, CW_EMO_ANGER, 0.7f);
 *   cw_face_set_mouth(f, open, shape);
 *   cw_face_update(f, dt);                 // writes the morph weights
 *
 * Ducking: cw_speech_duck() is 1 when no line plays and falls toward duck_level while one does (eased); multiply the
 * music's volume by it (cw_music_set_volume) so speech is heard over it.
 *
 * Settings: cw_speech_settings fills two rows for the shell's Settings screen (cw_shell_desc.settings_items):
 * "Voices" (volume, 0 to 10) and "Subtitles" (On, Off); cw_speech_volume_step and cw_speech_subtitles_on read them.
 *
 * Debug fields (cw_speech_publish(sp, "speech")): speech.lines (in the pack), speech.tone (1: tone stand-ins),
 * speech.played, speech.missing (lines asked for that the pack lacks), speech.playing (the key playing, 0 none),
 * speech.duck. A face's: cw_face_publish(f, "face.keeper") publishes face.keeper.w (every channel's weight, in
 * cw_face_channel order), face.keeper.found (a bit per channel found on the model), face.keeper.blinks. */
#ifndef CW_SPEECH_H
#define CW_SPEECH_H

#include "base.h"
#include "audio.h"
#include "story.h"
#include "three.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct cw_speech cw_speech;
typedef struct cw_shell_setting cw_shell_setting; /* cartwheel/shell.h */

cw_speech *cw_speech_load(const char *asset_path);              /* NULL (logged) when missing or malformed */
cw_speech *cw_speech_load_memory(const void *data, size_t size); /* copied */
void cw_speech_free(cw_speech *sp); /* frees the pack and its cached sounds (NULL is a no-op) */

int cw_speech_count(const cw_speech *sp); /* lines in the pack */
bool cw_speech_has(const cw_speech *sp, uint32_t key); /* the pack holds the line */
float cw_speech_seconds(const cw_speech *sp, uint32_t key);     /* 0 when the pack lacks the line */
uint32_t cw_speech_key(const char *cast_id, const char *text);  /* the key a line is filed under */
/* Play a line (desc NULL: the speech bus at volume 1). The decoded sound is cached (the last 8 lines). A line
 * started while another plays stops the other with a short fade. Returns the voice (0 when the pack lacks the line,
 * counted in speech.missing). */
cw_voice cw_speech_play(cw_speech *sp, uint32_t key, const cw_play_desc *desc);
void cw_speech_stop(cw_speech *sp, float fade); /* the line playing fades out over fade seconds */
/* The mouth at t seconds into the line: open 0..1, shape 0 (round) .. 1 (wide). 0, 0.5 outside the line or when the
 * pack lacks it. */
void cw_speech_mouth(const cw_speech *sp, uint32_t key, float t, float *open, float *shape);
/* Tell the pack which line sounds now (a story does this; a game playing lines itself may too): it drives the duck. */
void cw_speech_set_playing(cw_speech *sp, uint32_t key);
void cw_speech_tick(cw_speech *sp, float dt);   /* eases the duck; applies the Voices setting to the speech bus */
float cw_speech_duck(const cw_speech *sp);      /* 1 .. duck_level */
void cw_speech_set_duck_level(cw_speech *sp, float level); /* default 0.45 */
cw_bus cw_speech_bus(void);                     /* "speech", under the master; made on first use */

/* the Settings rows: rows[0] Voices, rows[1] Subtitles (saved under "speech_volume" and "speech_subtitles") */
void cw_speech_settings(cw_shell_setting *rows); /* rows[2] */
int cw_speech_volume_step(void);   /* 0..10, default 10 */
bool cw_speech_subtitles_on(void); /* default on */

void cw_speech_publish(cw_speech *sp, const char *prefix); /* debug fields under prefix (see the top of this header) */

/* faces */
typedef enum {
  CW_FACE_JAW_OPEN, CW_FACE_MOUTH_ROUND, CW_FACE_MOUTH_WIDE, CW_FACE_SMILE, CW_FACE_FROWN, CW_FACE_SNEER,
  CW_FACE_BROW_UP, CW_FACE_BROW_DOWN, CW_FACE_BROW_INNER, CW_FACE_EYES_WIDE, CW_FACE_SQUINT, CW_FACE_BLINK,
  CW_FACE_CHANNELS
} cw_face_channel;
/* a model's own target names per channel (NULL: try the defaults); several names may be given for one channel
 * separated by commas ("mouthSmile_L,mouthSmile_R"): each is set to the channel's weight */
typedef struct cw_face_map {
  const char *names[CW_FACE_CHANNELS];
  float scale[CW_FACE_CHANNELS];   /* the weight at a channel's full strength (0: 1) */
} cw_face_map;
typedef struct cw_face cw_face;
cw_face *cw_face_new(t3_object *root, const cw_face_map *map); /* a face on every mesh under root that has the morph targets (NULL map: the default names); NULL when root is NULL */
void cw_face_free(cw_face *f); /* frees the face (the model is the caller's) */
uint32_t cw_face_channels(const cw_face *f);    /* a bit per channel found on the model */
void cw_face_set_emotion(cw_face *f, cw_emotion e, float amount); /* the emotion the face eases toward, amount 0..1 */
void cw_face_set_mouth(cw_face *f, float open, float shape); /* the mouth now: open 0..1, shape 0 round .. 1 wide (cw_speech_mouth) */
void cw_face_set_channel(cw_face *f, cw_face_channel c, float extra); /* the game's own, added over the emotion */
void cw_face_set_blinking(cw_face *f, bool on, uint32_t seed); /* on by default; the seed spreads blinks between faces */
void cw_face_update(cw_face *f, float dt);       /* ease every channel and write the morph weights */
float cw_face_weight(const cw_face *f, cw_face_channel c); /* a channel's eased weight (0..1) after the last update */
void cw_face_publish(cw_face *f, const char *prefix); /* debug fields under prefix: .w (every channel), .found, .blinks */

/* One call per tick for a story's speakers: the speaking cast member's face takes the line's mouth and emotion,
 * every other face eases back to calm and a closed mouth. faces[i] is cast member i's face (NULL for those with
 * none). Call cw_face_update for each face after it (or let this do it: update = true). */
void cw_speech_drive_faces(const cw_story *st, const cw_speech *sp, cw_face *const *faces, int count, float dt, bool update);

#ifdef __cplusplus
}
#endif
#endif

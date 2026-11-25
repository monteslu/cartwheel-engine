/* speech.c: speech packs and faces (cartwheel/speech.h). The pack layout is tools/speech-bake.mjs's. The ADPCM
 * decoder and the mouth from the sound (open from the loudness, shape from the brightness) follow keyholder's voice.c
 * (github.com/virgilvox/keyholder, MIT, see CREDITS); here the mouth is baked per line in the pack and read by time,
 * so it is the same on every host whether or not audio plays. */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cartwheel/speech.h"
#include "cartwheel/debug.h"
#include "cartwheel/morph.h"
#include "cartwheel/platform.h"
#include "cartwheel/shell.h"

typedef struct { uint32_t key, rate, frames, aoff, alen, eoff, ecount; } entry_t;
#define CACHE_N 8

struct cw_speech {
  uint8_t *blob; size_t len;
  entry_t *e; int n; uint32_t env_rate; bool tone;
  const uint8_t *data; size_t data_len;
  struct { uint32_t key; cw_sound snd; uint32_t used; } cache[CACHE_N];
  uint32_t use_clock;
  cw_voice voice; uint32_t playing;
  float duck, duck_level;
  int32_t played, missing, dbg_lines, dbg_tone; uint32_t dbg_playing; float dbg_duck;
};

static uint32_t fnv_line(const char *who, const char *text) {
  uint32_t h = 2166136261u;
  for (const char *p = who; p && *p; p++) { h ^= (uint8_t)*p; h *= 16777619u; }
  h ^= '|'; h *= 16777619u;
  for (const char *p = text; p && *p; p++) { h ^= (uint8_t)*p; h *= 16777619u; }
  return h;
}
uint32_t cw_speech_key(const char *cast_id, const char *text) { return fnv_line(cast_id, text); }

static bool parse(cw_speech *sp) {
  if (sp->len < 20 || memcmp(sp->blob, "CWSP", 4)) return false;
  uint32_t ver, n, rate, flags;
  memcpy(&ver, sp->blob + 4, 4); memcpy(&n, sp->blob + 8, 4); memcpy(&rate, sp->blob + 12, 4); memcpy(&flags, sp->blob + 16, 4);
  if (ver != 1 || rate == 0 || 20 + (size_t)n * 28 > sp->len) return false;
  sp->n = (int)n; sp->env_rate = rate; sp->tone = flags & 1;
  sp->e = calloc(n + 1, sizeof *sp->e);
  sp->data = sp->blob + 20 + (size_t)n * 28; sp->data_len = sp->len - 20 - (size_t)n * 28;
  for (uint32_t i = 0; i < n; i++) {
    const uint8_t *p = sp->blob + 20 + (size_t)i * 28;
    entry_t *e = &sp->e[i];
    memcpy(e, p, 28);
    if ((size_t)e->aoff + e->alen > sp->data_len || (size_t)e->eoff + (size_t)e->ecount * 2 > sp->data_len || e->rate == 0) return false;
    if ((e->frames + 1) / 2 > e->alen) return false;
    if (i && sp->e[i - 1].key >= e->key) return false;
  }
  return true;
}

static cw_speech *finish(cw_speech *sp, const char *what) {
  if (!sp->blob || !parse(sp)) {
    cw_error("speech: %s is missing or not a speech pack (tools/speech-bake.mjs)", what);
    free(sp->blob); free(sp->e); free(sp);
    return NULL;
  }
  sp->duck = 1; sp->duck_level = 0.45f;
  sp->dbg_lines = sp->n; sp->dbg_tone = sp->tone;
  return sp;
}
cw_speech *cw_speech_load(const char *path) {
  cw_speech *sp = calloc(1, sizeof *sp);
  long n = path ? cw_asset_size(path) : -1;
  if (n > 0) { sp->blob = malloc((size_t)n); sp->len = (size_t)cw_asset_read(path, sp->blob, (size_t)n); }
  return finish(sp, path ? path : "(null)");
}
cw_speech *cw_speech_load_memory(const void *data, size_t size) {
  cw_speech *sp = calloc(1, sizeof *sp);
  if (data && size) { sp->blob = malloc(size); memcpy(sp->blob, data, size); sp->len = size; }
  return finish(sp, "(memory)");
}
void cw_speech_free(cw_speech *sp) {
  if (!sp) return;
  for (int i = 0; i < CACHE_N; i++) if (sp->cache[i].key) cw_sound_free(sp->cache[i].snd);
  free(sp->blob); free(sp->e); free(sp);
}

static const entry_t *find(const cw_speech *sp, uint32_t key) {
  if (!sp) return NULL;
  int lo = 0, hi = sp->n;
  while (lo < hi) { int m = (lo + hi) / 2; if (sp->e[m].key < key) lo = m + 1; else hi = m; }
  return lo < sp->n && sp->e[lo].key == key ? &sp->e[lo] : NULL;
}
int cw_speech_count(const cw_speech *sp) { return sp ? sp->n : 0; }
bool cw_speech_has(const cw_speech *sp, uint32_t key) { return find(sp, key) != NULL; }
float cw_speech_seconds(const cw_speech *sp, uint32_t key) { const entry_t *e = find(sp, key); return e ? (float)e->frames / (float)e->rate : 0; }

static float *decode(const cw_speech *sp, const entry_t *e) {
  static const int16_t STEP[89] = { 7, 8, 9, 10, 11, 12, 13, 14, 16, 17, 19, 21, 23, 25, 28, 31, 34, 37, 41, 45, 50, 55, 60, 66, 73, 80, 88, 97,
    107, 118, 130, 143, 157, 173, 190, 209, 230, 253, 279, 307, 337, 371, 408, 449, 494, 544, 598, 658, 724, 796, 876, 963, 1060, 1166, 1282,
    1411, 1552, 1707, 1878, 2066, 2272, 2499, 2749, 3024, 3327, 3660, 4026, 4428, 4871, 5358, 5894, 6484, 7132, 7845, 8630, 9493, 10442, 11487,
    12635, 13899, 15289, 16818, 18500, 20350, 22385, 24623, 27086, 29794, 32767 };
  static const int8_t IDX[8] = { -1, -1, -1, -1, 2, 4, 6, 8 };
  float *o = malloc((size_t)e->frames * sizeof(float));
  if (!o) return NULL;
  int pred = 0, idx = 0;
  const uint8_t *d = sp->data + e->aoff;
  for (uint32_t i = 0; i < e->frames; i++) {
    int code = (i & 1) ? d[i >> 1] >> 4 : d[i >> 1] & 15, step = STEP[idx], diff = step >> 3;
    if (code & 4) diff += step;
    if (code & 2) diff += step >> 1;
    if (code & 1) diff += step >> 2;
    pred += code & 8 ? -diff : diff;
    pred = pred < -32768 ? -32768 : pred > 32767 ? 32767 : pred;
    idx += IDX[code & 7]; idx = idx < 0 ? 0 : idx > 88 ? 88 : idx;
    o[i] = (float)pred / 32768.f;
  }
  return o;
}

static cw_bus speech_bus = -1;
cw_bus cw_speech_bus(void) {
  if (speech_bus < 0 && cw_audio_sample_rate() > 0) {
    speech_bus = cw_bus_find("speech");
    if (speech_bus < 0) speech_bus = cw_bus_create("speech", 0);
  }
  return speech_bus < 0 ? 0 : speech_bus;
}

static cw_sound sound_for(cw_speech *sp, const entry_t *e) {
  int slot = -1; uint32_t oldest = UINT32_MAX;
  for (int i = 0; i < CACHE_N; i++) {
    if (sp->cache[i].key == e->key) { sp->cache[i].used = ++sp->use_clock; return sp->cache[i].snd; }
    if (sp->cache[i].key == sp->playing && sp->cache[i].key) continue; /* never evict the sound playing */
    if (sp->cache[i].used < oldest) { oldest = sp->cache[i].used; slot = i; }
  }
  if (slot < 0) slot = 0;
  float *pcm = decode(sp, e);
  if (!pcm) return (cw_sound){ 0 };
  cw_sound s = cw_sound_from_pcm(pcm, (int)e->frames, 1, (int)e->rate, NULL);
  free(pcm);
  if (sp->cache[slot].key) cw_sound_free(sp->cache[slot].snd);
  sp->cache[slot].key = e->key; sp->cache[slot].snd = s; sp->cache[slot].used = ++sp->use_clock;
  return s;
}

cw_voice cw_speech_play(cw_speech *sp, uint32_t key, const cw_play_desc *desc) {
  if (!sp) return (cw_voice){ 0 };
  const entry_t *e = find(sp, key);
  if (!e) { sp->missing++; cw_warn("speech: the pack has no line %08x", key); return (cw_voice){ 0 }; }
  sp->played++;
  if (cw_audio_sample_rate() <= 0) return (cw_voice){ 0 }; /* no mixer: the line is timed and its mouth moves, silently */
  if (sp->voice.id) cw_voice_stop(sp->voice, 0.05f);
  cw_sound s = sound_for(sp, e);
  if (CW_HANDLE_IS_NULL(s)) return (cw_voice){ 0 };
  cw_play_desc pd = desc ? *desc : (cw_play_desc){ 0 };
  if (!desc) pd.bus = cw_speech_bus();
  sp->voice = cw_audio_play(s, &pd);
  return sp->voice;
}
void cw_speech_stop(cw_speech *sp, float fade) { if (sp && sp->voice.id) { cw_voice_stop(sp->voice, fade); sp->voice = (cw_voice){ 0 }; } }

void cw_speech_mouth(const cw_speech *sp, uint32_t key, float t, float *open, float *shape) {
  float o = 0, s = 0.5f;
  const entry_t *e = find(sp, key);
  if (e && e->ecount && t >= 0) {
    float x = t * (float)sp->env_rate;
    uint32_t i = (uint32_t)x; float f = x - (float)i;
    if (i < e->ecount) {
      const uint8_t *p = sp->data + e->eoff;
      uint32_t j = i + 1 < e->ecount ? i + 1 : i;
      o = ((float)p[i * 2] * (1 - f) + (float)p[j * 2] * f) / 255.f;
      s = ((float)p[i * 2 + 1] * (1 - f) + (float)p[j * 2 + 1] * f) / 255.f;
    }
  }
  if (open) *open = o;
  if (shape) *shape = s;
}

void cw_speech_set_playing(cw_speech *sp, uint32_t key) { if (sp) sp->playing = key; }
void cw_speech_set_duck_level(cw_speech *sp, float level) { if (sp) sp->duck_level = level; }
float cw_speech_duck(const cw_speech *sp) { return sp ? sp->duck : 1; }

/* the settings rows */
static int vol_step = 10, subs_ix = 0;
static const char *const VOL_CHOICES[11] = { "0", "1", "2", "3", "4", "5", "6", "7", "8", "9", "10" };
static const char *const SUB_CHOICES[2] = { "On", "Off" };
void cw_speech_settings(cw_shell_setting *rows) {
  rows[0] = (cw_shell_setting){ .label = "Voices", .key = "speech_volume", .value = &vol_step, .choices = VOL_CHOICES, .count = 11 };
  rows[1] = (cw_shell_setting){ .label = "Subtitles", .key = "speech_subtitles", .value = &subs_ix, .choices = SUB_CHOICES, .count = 2 };
}
int cw_speech_volume_step(void) { return vol_step < 0 ? 0 : vol_step > 10 ? 10 : vol_step; }
bool cw_speech_subtitles_on(void) { return subs_ix == 0; }

void cw_speech_tick(cw_speech *sp, float dt) {
  if (!sp) return;
  float target = sp->playing ? sp->duck_level : 1;
  float k = 1 - expf(-(sp->playing ? 8.f : 2.5f) * dt); /* down quickly, back up slowly */
  sp->duck += (target - sp->duck) * k;
  if (cw_audio_sample_rate() > 0) cw_bus_set_volume(cw_speech_bus(), (float)cw_speech_volume_step() / 10.f);
  sp->dbg_playing = sp->playing; sp->dbg_duck = sp->duck;
}

static const char *dname(const char *prefix, const char *leaf) {
  size_t n = strlen(prefix) + strlen(leaf) + 2;
  char *s = malloc(n); snprintf(s, n, "%s.%s", prefix, leaf);
  return s; /* the debug channel keeps the pointer */
}
void cw_speech_publish(cw_speech *sp, const char *prefix) {
  if (!sp) return;
  if (!prefix) prefix = "speech";
  cw_debug_i32(dname(prefix, "lines"), &sp->dbg_lines);
  cw_debug_i32(dname(prefix, "tone"), &sp->dbg_tone);
  cw_debug_i32(dname(prefix, "played"), &sp->played);
  cw_debug_i32(dname(prefix, "missing"), &sp->missing);
  cw_debug_u32(dname(prefix, "playing"), &sp->dbg_playing);
  cw_debug_f32(dname(prefix, "duck"), &sp->dbg_duck);
}

/* ---- faces ---- */
static const char *const DEFAULT_NAMES[CW_FACE_CHANNELS][8] = {
  [CW_FACE_JAW_OPEN] = { "jawOpen", "JawOpen", "mouthOpen", "MouthOpen", "aa", "A", "viseme_aa", "Fcl_MTH_A" },
  [CW_FACE_MOUTH_ROUND] = { "mouthFunnel", "mouthPucker", "oh", "ou", "O", "viseme_O", "Fcl_MTH_O", "mouthRound" },
  [CW_FACE_MOUTH_WIDE] = { "mouthStretch_L,mouthStretch_R", "mouthWide", "ee", "ih", "E", "viseme_E", "Fcl_MTH_E", NULL },
  [CW_FACE_SMILE] = { "mouthSmile_L,mouthSmile_R", "mouthSmile", "smile", "Smile", "happy", "Joy", "Fcl_ALL_Joy", NULL },
  [CW_FACE_FROWN] = { "mouthFrown_L,mouthFrown_R", "mouthFrown", "frown", "Frown", "sad", "Sorrow", "Fcl_ALL_Sorrow", NULL },
  [CW_FACE_SNEER] = { "noseSneer_L,noseSneer_R", "noseSneer", "sneer", "Sneer", "angry", "Angry", "Fcl_ALL_Angry", NULL },
  [CW_FACE_BROW_UP] = { "browOuterUp_L,browOuterUp_R", "browUp", "BrowUp", "browsUp", "surprised", "Surprised", NULL, NULL },
  [CW_FACE_BROW_DOWN] = { "browDown_L,browDown_R", "browDown", "BrowDown", "browsDown", NULL, NULL, NULL, NULL },
  [CW_FACE_BROW_INNER] = { "browInnerUp", "browInner", "BrowInner", NULL, NULL, NULL, NULL, NULL },
  [CW_FACE_EYES_WIDE] = { "eyeWide_L,eyeWide_R", "eyesWide", "eyeWide", "EyesWide", NULL, NULL, NULL, NULL },
  [CW_FACE_SQUINT] = { "eyeSquint_L,eyeSquint_R", "squint", "Squint", NULL, NULL, NULL, NULL, NULL },
  [CW_FACE_BLINK] = { "eyeBlink_L,eyeBlink_R", "blink", "Blink", "eyesClosed", "Fcl_EYE_Close", NULL, NULL, NULL },
};
/* each emotion's face at full strength */
static const float EMO[CW_EMO_COUNT][CW_FACE_CHANNELS] = {
  [CW_EMO_CALM] = { 0 },
  [CW_EMO_JOY] = { [CW_FACE_SMILE] = .85f, [CW_FACE_BROW_UP] = .15f, [CW_FACE_SQUINT] = .3f },
  [CW_EMO_ANGER] = { [CW_FACE_BROW_DOWN] = .9f, [CW_FACE_FROWN] = .35f, [CW_FACE_SNEER] = .6f, [CW_FACE_SQUINT] = .3f },
  [CW_EMO_SORROW] = { [CW_FACE_FROWN] = .75f, [CW_FACE_BROW_INNER] = .85f },
  [CW_EMO_FEAR] = { [CW_FACE_BROW_INNER] = .6f, [CW_FACE_BROW_UP] = .7f, [CW_FACE_EYES_WIDE] = .7f, [CW_FACE_MOUTH_WIDE] = .25f },
  [CW_EMO_SURPRISE] = { [CW_FACE_BROW_UP] = 1, [CW_FACE_EYES_WIDE] = .8f, [CW_FACE_JAW_OPEN] = .2f, [CW_FACE_MOUTH_ROUND] = .3f },
};

struct cw_face {
  t3_object *root;
  char *names[CW_FACE_CHANNELS][2];
  float scale[CW_FACE_CHANNELS];
  uint32_t found;
  float w[CW_FACE_CHANNELS], extra[CW_FACE_CHANNELS];
  cw_emotion emo; float emo_amt; float open, shape;
  bool blinking; uint32_t rng; float blink_next, blink_t;
  int32_t blinks, found_dbg;
};

static bool has_name(const char **names, int n, const char *s, size_t len) {
  for (int i = 0; i < n; i++) if (strlen(names[i]) == len && !strncmp(names[i], s, len)) return true;
  return false;
}
/* a candidate "a,b": every part present */
static bool candidate(const char **names, int n, const char *c, char *out[2]) {
  const char *comma = strchr(c, ',');
  size_t l0 = comma ? (size_t)(comma - c) : strlen(c);
  if (!has_name(names, n, c, l0)) return false;
  if (comma && !has_name(names, n, comma + 1, strlen(comma + 1))) return false;
  out[0] = malloc(l0 + 1); memcpy(out[0], c, l0); out[0][l0] = 0;
  if (comma) { size_t l1 = strlen(comma + 1); out[1] = malloc(l1 + 1); memcpy(out[1], comma + 1, l1 + 1); } else out[1] = NULL;
  return true;
}
static float frand(cw_face *f) { f->rng = f->rng * 1664525u + 1013904223u; return (float)(f->rng >> 8) / 16777216.f; }

cw_face *cw_face_new(t3_object *root, const cw_face_map *map) {
  if (!root) return NULL;
  cw_face *f = calloc(1, sizeof *f);
  f->root = root;
  const char *names[256];
  int n = cw_morph_names(root, names, 256);
  if (n > 256) n = 256;
  for (int c = 0; c < CW_FACE_CHANNELS; c++) {
    f->scale[c] = map && map->scale[c] > 0 ? map->scale[c] : 1;
    if (map && map->names[c]) { if (candidate(names, n, map->names[c], f->names[c])) f->found |= 1u << c; continue; }
    for (int k = 0; k < 8 && DEFAULT_NAMES[c][k]; k++) if (candidate(names, n, DEFAULT_NAMES[c][k], f->names[c])) { f->found |= 1u << c; break; }
  }
  f->blinking = true; f->rng = 0x9e3779b9u; f->blink_next = 1.5f + 2.5f * frand(f);
  f->shape = 0.5f; f->found_dbg = (int32_t)f->found;
  return f;
}
void cw_face_free(cw_face *f) {
  if (!f) return;
  for (int c = 0; c < CW_FACE_CHANNELS; c++) { free(f->names[c][0]); free(f->names[c][1]); }
  free(f);
}
uint32_t cw_face_channels(const cw_face *f) { return f ? f->found : 0; }
void cw_face_set_emotion(cw_face *f, cw_emotion e, float amount) { if (f) { f->emo = e < 0 || e >= CW_EMO_COUNT ? CW_EMO_CALM : e; f->emo_amt = amount < 0 ? 0 : amount > 1 ? 1 : amount; } }
void cw_face_set_mouth(cw_face *f, float open, float shape) { if (f) { f->open = open; f->shape = shape; } }
void cw_face_set_channel(cw_face *f, cw_face_channel c, float extra) { if (f && c >= 0 && c < CW_FACE_CHANNELS) f->extra[c] = extra; }
void cw_face_set_blinking(cw_face *f, bool on, uint32_t seed) {
  if (!f) return;
  f->blinking = on; f->rng = seed * 2654435761u + 1; f->blink_next = 1.0f + 3.5f * frand(f); f->blink_t = -1;
}
float cw_face_weight(const cw_face *f, cw_face_channel c) { return f && c >= 0 && c < CW_FACE_CHANNELS ? f->w[c] : 0; }

void cw_face_update(cw_face *f, float dt) {
  if (!f) return;
  float target[CW_FACE_CHANNELS];
  for (int c = 0; c < CW_FACE_CHANNELS; c++) target[c] = EMO[f->emo][c] * f->emo_amt + f->extra[c];
  /* a channel the model lacks lends its weight to a near one */
  if (!(f->found & (1u << CW_FACE_BROW_INNER))) target[CW_FACE_BROW_UP] += target[CW_FACE_BROW_INNER] * .6f;
  if (!(f->found & (1u << CW_FACE_SNEER))) target[CW_FACE_FROWN] += target[CW_FACE_SNEER] * .5f;
  if (!(f->found & (1u << CW_FACE_EYES_WIDE))) target[CW_FACE_BROW_UP] += target[CW_FACE_EYES_WIDE] * .3f;
  if (!(f->found & (1u << CW_FACE_BROW_DOWN))) target[CW_FACE_FROWN] += target[CW_FACE_BROW_DOWN] * .3f;
  /* the mouth: the jaw opens with the sound, round or wide by its shape; a smile keeps its corners while talking */
  float o = f->open < 0 ? 0 : f->open > 1 ? 1 : f->open, sh = f->shape < 0 ? 0 : f->shape > 1 ? 1 : f->shape;
  target[CW_FACE_JAW_OPEN] += o * .85f;
  target[CW_FACE_MOUTH_ROUND] += o * (1 - sh) * .55f;
  target[CW_FACE_MOUTH_WIDE] += o * sh * .45f;
  if (!(f->found & ((1u << CW_FACE_MOUTH_ROUND) | (1u << CW_FACE_MOUTH_WIDE)))) target[CW_FACE_JAW_OPEN] += o * .1f;
  /* blinks: closed over 0.05 s, held 0.03 s, opened over 0.08 s, every 1 to 4.5 s */
  float blink = 0;
  if (f->blinking) {
    if (f->blink_t < 0) { f->blink_next -= dt; if (f->blink_next <= 0) { f->blink_t = 0; f->blinks++; } }
    else {
      f->blink_t += dt;
      float t = f->blink_t;
      blink = t < .05f ? t / .05f : t < .08f ? 1 : t < .16f ? 1 - (t - .08f) / .08f : 0;
      if (t >= .16f) { f->blink_t = -1; f->blink_next = 1.0f + 3.5f * frand(f); }
    }
  }
  target[CW_FACE_BLINK] += blink;
  for (int c = 0; c < CW_FACE_CHANNELS; c++) {
    float tv = target[c] < 0 ? 0 : target[c] > 1 ? 1 : target[c];
    bool fast = c == CW_FACE_JAW_OPEN || c == CW_FACE_MOUTH_ROUND || c == CW_FACE_MOUTH_WIDE;
    if (c == CW_FACE_BLINK) f->w[c] = tv;
    else f->w[c] += (tv - f->w[c]) * (1 - expf(-(fast ? 22.f : 5.f) * dt));
    if (f->found & (1u << c)) {
      cw_morph_set(f->root, f->names[c][0], f->w[c] * f->scale[c]);
      if (f->names[c][1]) cw_morph_set(f->root, f->names[c][1], f->w[c] * f->scale[c]);
    }
  }
}
void cw_face_publish(cw_face *f, const char *prefix) {
  if (!f || !prefix) return;
  cw_debug_f32_array(dname(prefix, "w"), f->w, CW_FACE_CHANNELS);
  cw_debug_i32(dname(prefix, "found"), &f->found_dbg);
  cw_debug_i32(dname(prefix, "blinks"), &f->blinks);
}

void cw_speech_drive_faces(const cw_story *st, const cw_speech *sp, cw_face *const *faces, int count, float dt, bool update) {
  int who = -1; uint32_t key = 0; float t = 0, amt = 0; cw_emotion emo = CW_EMO_CALM;
  bool speaking = st && cw_story_speaking(st, &who, &key, &t, &emo, &amt);
  for (int i = 0; i < count; i++) {
    cw_face *f = faces[i];
    if (!f) continue;
    if (speaking && i == who) {
      float o = 0, s = 0.5f;
      if (sp && cw_speech_has(sp, key)) cw_speech_mouth(sp, key, t, &o, &s);
      else { float k = sinf(t * 9.4f); o = k > 0 ? k * .7f : 0; s = .5f + .3f * sinf(t * 3.1f); } /* no voice: a talking rhythm */
      cw_face_set_mouth(f, o, s);
      cw_face_set_emotion(f, emo, amt);
    } else {
      cw_face_set_mouth(f, 0, 0.5f);
      if (speaking) cw_face_set_emotion(f, CW_EMO_CALM, 0);
    }
    if (update) cw_face_update(f, dt);
  }
}

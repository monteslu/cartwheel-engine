/* Randomized sounds, interactive music and their .cwaudio resources (cartwheel/audio.h).
 *
 * Both run on the game thread over the mixer's public calls. Interactive music keeps every clip on the mixer's
 * frame clock: a clip's beat 0 is the mixer frame its voice started on (cw_voice_started_at), its beats and bars
 * are fixed frame counts from there, and a switch is a new voice with a start_frame and the old voice ended on that
 * same frame (cw_voice_stop_at), so the seam is exact to the sample whatever the game's frame rate. Integer frames
 * from double arithmetic only (+ - * / and floor, the same on every host): an offline mixer switches on the same
 * samples everywhere. */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cartwheel/audio.h"
#include "cartwheel/core.h"
#include "cartwheel/math.h"
#include "cartwheel/platform.h"

/* ── randomizer ─────────────────────────────────────────────────────── */

typedef struct randomizer {
  cw_randomizer_desc d;
  cw_rng rng;
  cw_sound sounds[CW_RANDOMIZER_MAX];
  float weights[CW_RANDOMIZER_MAX];
  int n, last, seq;
} randomizer;

static cw_pool rpool, mpool;
static bool pools_ready;
static void pools_init(void) {
  if (pools_ready) return;
  cw_pool_init(&rpool, sizeof(randomizer), CW_MEM_AUDIO);
  cw_pool_init(&mpool, sizeof(struct music *), CW_MEM_AUDIO);
  pools_ready = true;
}

cw_randomizer cw_randomizer_new(const cw_randomizer_desc *desc) {
  pools_init();
  uint64_t id;
  randomizer *r = cw_pool_alloc(&rpool, &id);
  r->d = desc ? *desc : (cw_randomizer_desc){ 0 };
  if (r->d.random_pitch < 1) r->d.random_pitch = 1;
  if (r->d.random_volume_db < 0) r->d.random_volume_db = -r->d.random_volume_db;
  cw_rng_seed(&r->rng, r->d.seed);
  r->last = -1;
  return (cw_randomizer){ id };
}
static randomizer *rget(cw_randomizer h) { return pools_ready ? cw_pool_get(&rpool, h.id) : NULL; }
void cw_randomizer_free(cw_randomizer h) {
  if (rget(h)) cw_pool_release(&rpool, h.id);
}
int cw_randomizer_add(cw_randomizer h, cw_sound s, float weight) {
  randomizer *r = rget(h);
  if (!r || r->n >= CW_RANDOMIZER_MAX || CW_HANDLE_IS_NULL(s)) return -1;
  r->sounds[r->n] = s;
  r->weights[r->n] = weight > 0 ? weight : 1;
  return r->n++;
}
int cw_randomizer_count(cw_randomizer h) {
  randomizer *r = rget(h);
  return r ? r->n : 0;
}
int cw_randomizer_last(cw_randomizer h) {
  randomizer *r = rget(h);
  return r ? r->last : -1;
}
cw_sound cw_randomizer_next(cw_randomizer h, float *pitch, float *volume) {
  randomizer *r = rget(h);
  if (pitch) *pitch = 1;
  if (volume) *volume = 1;
  if (!r || r->n == 0) return (cw_sound){ 0 };
  int pick = 0;
  if (r->d.mode == CW_RANDOM_SEQUENCE) {
    pick = r->seq % r->n;
    r->seq = (r->seq + 1) % r->n;
  } else {
    bool skip = r->d.mode == CW_RANDOM_NO_REPEAT && r->n > 1 && r->last >= 0;
    float total = 0;
    for (int i = 0; i < r->n; i++)
      if (!(skip && i == r->last)) total += r->weights[i];
    float x = cw_rng_float(&r->rng) * total;
    pick = -1;
    for (int i = 0; i < r->n; i++) {
      if (skip && i == r->last) continue;
      pick = i;
      if (x < r->weights[i]) break;
      x -= r->weights[i];
    }
  }
  r->last = pick;
  /* one rng call per statement (the determinism rule), in a fixed order */
  float u = r->d.random_pitch > 1 ? cw_rng_float(&r->rng) : 0;
  float v = r->d.random_volume_db > 0 ? cw_rng_float(&r->rng) : 0.5f;
  if (pitch && r->d.random_pitch > 1) {
    float lo = 1.0f / r->d.random_pitch, hi = r->d.random_pitch;
    *pitch = lo + (hi - lo) * u;
  }
  if (volume && r->d.random_volume_db > 0) *volume = cw_exp((v * 2 - 1) * r->d.random_volume_db * 0.115129255f); /* 10^(db/20) */
  return r->sounds[pick];
}
cw_voice cw_randomizer_play(cw_randomizer h, const cw_play_desc *desc) {
  float pitch, vol;
  cw_sound s = cw_randomizer_next(h, &pitch, &vol);
  if (CW_HANDLE_IS_NULL(s)) return (cw_voice){ 0 };
  cw_play_desc d = desc ? *desc : (cw_play_desc){ 0 };
  d.pitch = (d.pitch > 0 ? d.pitch : 1) * pitch;
  d.volume = (d.volume > 0 ? d.volume : 1) * vol;
  return cw_audio_play(s, &d);
}

/* ── interactive music ──────────────────────────────────────────────── */

typedef struct clip {
  char name[48];
  cw_music_clip c;
  char next[48];
} clip;
typedef struct rule {
  int from, to, filler; /* -1 = any / none */
  cw_music_transition t;
} rule;

typedef struct music {
  cw_music_desc d;
  clip clips[CW_MUSIC_MAX_CLIPS];
  int nclips;
  rule rules[CW_MUSIC_MAX_TRANSITIONS];
  int nrules;
  bool spatial, paused;
  cw_voice_3d space;
  /* playing */
  int cur;             /* -1 = stopped */
  cw_voice voice;
  int64_t start;       /* mixer frame of the current clip's beat 0; -1 until its voice has started */
  int64_t start_off;   /* mixer frames into the clip the voice started at (beat 0 = started_at - start_off) */
  bool advanced;       /* the current clip's own advance is scheduled */
  int hold, hold_start_valid;
  int64_t hold_start;
  /* a switch */
  int target;          /* the clip asked for; -1 none */
  int after;           /* through a filler: the clip after it */
  bool scheduled;
  int64_t at;          /* mixer frame of the switch */
  int64_t next_off;    /* mixer frames into the new clip it starts at */
  int next;
  cw_voice next_voice;
  bool next_returning;
} music;

static music *mget(cw_music h) {
  if (!pools_ready) return NULL;
  music **pp = cw_pool_get(&mpool, h.id);
  return pp ? *pp : NULL;
}
static int find_clip(const music *m, const char *name) {
  if (!name) return -1;
  for (int i = 0; i < m->nclips; i++)
    if (!strcmp(m->clips[i].name, name)) return i;
  return -1;
}

cw_music cw_music_new(const cw_music_desc *desc) {
  pools_init();
  uint64_t id;
  music **pp = cw_pool_alloc(&mpool, &id);
  music *m = cw_calloc(1, sizeof *m, CW_MEM_AUDIO);
  *pp = m;
  m->d = desc ? *desc : (cw_music_desc){ 0 };
  if (!(m->d.volume > 0)) m->d.volume = 1;
  m->cur = m->target = m->after = m->hold = -1;
  m->start = -1;
  return (cw_music){ id };
}
void cw_music_free(cw_music h) {
  music *m = mget(h);
  if (!m) return;
  cw_voice_stop(m->voice, 0);
  cw_voice_stop(m->next_voice, 0);
  cw_free(m);
  cw_pool_release(&mpool, h.id);
}
int cw_music_add_clip(cw_music h, const char *name, const cw_music_clip *c) {
  music *m = mget(h);
  if (!m || !name || !c || m->nclips >= CW_MUSIC_MAX_CLIPS || find_clip(m, name) >= 0 || CW_HANDLE_IS_NULL(c->sound)) return -1;
  clip *k = &m->clips[m->nclips];
  snprintf(k->name, sizeof k->name, "%s", name);
  k->c = *c;
  if (k->c.bar_beats <= 0) k->c.bar_beats = 4;
  snprintf(k->next, sizeof k->next, "%s", c->next ? c->next : "");
  k->c.next = NULL;
  return m->nclips++;
}
bool cw_music_add_transition(cw_music h, const char *from, const char *to, const cw_music_transition *t) {
  music *m = mget(h);
  if (!m || !t || m->nrules >= CW_MUSIC_MAX_TRANSITIONS) return false;
  rule r = { .from = from ? find_clip(m, from) : -1, .to = to ? find_clip(m, to) : -1, .filler = t->filler ? find_clip(m, t->filler) : -1,
             .t = *t };
  if ((from && r.from < 0) || (to && r.to < 0) || (t->filler && r.filler < 0)) {
    cw_error("music: transition %s -> %s names a clip that is not added (%s)", from ? from : "any", to ? to : "any",
             t->filler ? t->filler : "");
    return false;
  }
  r.t.filler = NULL;
  m->rules[m->nrules++] = r;
  return true;
}

static int mix_rate(void) { return cw_audio_sample_rate(); }
static double beat_frames(const clip *k) { return k->c.bpm > 0 ? 60.0 * mix_rate() / k->c.bpm : 0; }
/* the clip's first pass and its loop period after that, in mixer frames (-1 unknown) */
static void clip_lengths(const clip *k, int64_t *first, int64_t *period) {
  int sr = cw_sound_source_rate(k->c.sound), mr = mix_rate();
  int64_t ls = 0, le = 0;
  bool own = cw_sound_get_loop(k->c.sound, &ls, &le);
  double sr_to_mr = sr > 0 ? (double)mr / sr : 1;
  if (k->c.beats > 0 && k->c.bpm > 0) *first = (int64_t)floor(k->c.beats * beat_frames(k) + 0.5);
  else {
    int64_t len = cw_sound_length_frames(k->c.sound);
    *first = own && le > 0 ? (int64_t)floor(le * sr_to_mr + 0.5) : len >= 0 ? (int64_t)floor(len * sr_to_mr + 0.5) : -1;
  }
  int64_t lsm = own ? (int64_t)floor(ls * sr_to_mr + 0.5) : 0;
  *period = *first > lsm ? *first - lsm : *first;
}

static const rule *find_rule(const music *m, int from, int to) {
  const rule *best = NULL;
  int score = -1;
  for (int i = 0; i < m->nrules; i++) {
    const rule *r = &m->rules[i];
    if ((r->from >= 0 && r->from != from) || (r->to >= 0 && r->to != to)) continue;
    int s = (r->from >= 0 ? 1 : 0) + (r->to >= 0 ? 2 : 0); /* from+to 3, to 2, from 1, any 0 */
    if (s > score) score = s, best = r;
  }
  return best;
}

static void play_clip(music *m, int idx, int64_t at, int64_t off_mix, float fade_in) {
  clip *k = &m->clips[idx];
  int sr = cw_sound_source_rate(k->c.sound), mr = mix_rate();
  cw_play_desc d = { .volume = m->d.volume, .bus = m->d.bus, .start_frame = at, .fade_in = fade_in, .paused = m->paused,
                     .spatial = m->spatial, .space = m->space };
  d.offset = sr > 0 ? (int64_t)floor(off_mix * ((double)sr / mr) + 0.5) : 0;
  if (k->c.loop) {
    d.loop = true;
    if (k->c.beats > 0 && k->c.bpm > 0) { /* the clip's beats are its loop: from the sound's loop start (else 0) */
      int64_t ls = 0, le = 0;
      cw_sound_get_loop(k->c.sound, &ls, &le);
      d.loop_start = ls;
      d.loop_end = (int64_t)floor(k->c.beats * 60.0 * sr / k->c.bpm + 0.5);
      if (d.loop_end <= d.loop_start) d.loop_start = 0;
    }
  } else d.once = true;
  m->next_voice = cw_audio_play(k->c.sound, &d);
}

/* the first boundary at or after f0 on a grid from `start` every `step` frames (step <= 0: f0 itself) */
static int64_t grid_next(int64_t start, double step, int64_t f0) {
  if (step <= 0 || f0 <= start) return f0 <= start ? start : f0;
  double k = ceil((double)(f0 - start) / step);
  return start + (int64_t)floor(k * step + 0.5);
}

/* schedule the switch to m->target (the current clip's beat 0 is known, or nothing plays) */
static void schedule(music *m) {
  int64_t now = cw_audio_mixed_frames(), f0 = now + CW_MUSIC_LEAD;
  int to = m->target;
  if (m->cur < 0) { /* nothing plays: start it */
    play_clip(m, to, f0, 0, 0);
    m->at = f0;
    m->next_off = 0;
    m->next = to;
    m->scheduled = true;
    return;
  }
  clip *k = &m->clips[m->cur];
  cw_music_transition t = { .at = k->c.bpm > 0 ? CW_MUSIC_AT_BAR : CW_MUSIC_AT_NOW, .fade = CW_MUSIC_FADE_CROSS, .fade_beats = 1 };
  int filler = -1;
  if (m->after >= 0 && to == m->after) { /* out of a filler: at its end, a cut */
    t = (cw_music_transition){ .at = CW_MUSIC_AT_END, .fade = CW_MUSIC_FADE_NONE };
    m->after = -1;
  } else if (m->next_returning) {
    t = (cw_music_transition){ .at = CW_MUSIC_AT_END, .fade = CW_MUSIC_FADE_NONE, .same_position = true };
  } else if (m->advanced) {
    t = (cw_music_transition){ .at = CW_MUSIC_AT_END, .fade = CW_MUSIC_FADE_NONE };
  } else {
    const rule *r = find_rule(m, m->cur, to);
    if (r) t = r->t, filler = r->filler;
  }
  if (!(t.fade_beats > 0)) t.fade_beats = 1;
  double beat = beat_frames(k);
  int64_t first, period;
  clip_lengths(k, &first, &period);
  int64_t at = f0;
  switch (t.at) {
  case CW_MUSIC_AT_NOW: at = f0; break;
  case CW_MUSIC_AT_BEAT: at = grid_next(m->start, beat, f0); break;
  case CW_MUSIC_AT_BAR: at = grid_next(m->start, beat * k->c.bar_beats, f0); break;
  case CW_MUSIC_AT_END:
    if (first < 0) at = f0;
    else if (f0 <= m->start + first || !k->c.loop) at = CW_MAX(f0, m->start + first);
    else at = m->start + first + (period > 0 ? (int64_t)ceil((double)(f0 - m->start - first) / period) * period : 0);
    break;
  }
  int dest = filler >= 0 ? filler : to;
  if (filler >= 0) m->after = to;
  /* where the new clip starts: from its start, or at the same time into it as the old one is */
  int64_t off = 0;
  if (t.same_position) {
    int64_t base = m->next_returning && m->hold_start_valid ? m->hold_start : m->start;
    int64_t o = at - base, nf, np;
    clip_lengths(&m->clips[dest], &nf, &np);
    if (nf > 0 && o >= nf) off = m->clips[dest].c.loop && np > 0 ? nf - np + (o - nf) % np : 0;
    else off = o > 0 ? o : 0;
  }
  float fade_s = (float)((beat > 0 ? beat * t.fade_beats : 0.5 * t.fade_beats * mix_rate()) / mix_rate());
  bool fade_out = t.fade == CW_MUSIC_FADE_CROSS || t.fade == CW_MUSIC_FADE_OUT;
  bool fade_in = t.fade == CW_MUSIC_FADE_CROSS || t.fade == CW_MUSIC_FADE_IN;
  if (t.hold_previous) {
    m->hold = m->cur;
    m->hold_start = m->start;
    m->hold_start_valid = true;
  }
  play_clip(m, dest, at, off, fade_in ? fade_s : 0);
  cw_voice_stop_at(m->voice, at, fade_out ? fade_s : 0);
  m->at = at;
  m->next_off = off;
  m->next = dest;
  m->scheduled = true;
}

static void update_one(music *m) {
  if (m->paused) return;
  int64_t now = cw_audio_mixed_frames();
  /* the current clip's beat 0, once its voice has started */
  if (m->cur >= 0 && m->start < 0) {
    int64_t s = cw_voice_started_at(m->voice);
    if (s >= 0) m->start = s - m->start_off;
  }
  /* a switch that has happened */
  if (m->scheduled && now >= m->at) {
    m->cur = m->next;
    m->voice = m->next_voice;
    m->next_voice = (cw_voice){ 0 };
    int64_t s = cw_voice_started_at(m->voice);
    m->start_off = m->next_off;
    m->start = s >= 0 ? s - m->next_off : -1;
    m->scheduled = false;
    m->advanced = false;
    m->next_returning = false;
    if (m->cur == m->target) m->target = -1;
    if (m->after >= 0) m->target = m->after; /* through a filler: on to the clip asked for, at the filler's end */
  }
  if (m->cur >= 0 && !cw_voice_is_playing(m->voice) && !m->scheduled && m->target < 0) m->cur = -1; /* it ended */
  /* the current clip advances by itself at its end */
  if (m->cur >= 0 && m->target < 0 && !m->scheduled && !m->advanced) {
    clip *k = &m->clips[m->cur];
    int dest = -1;
    if (k->c.advance == CW_MUSIC_ADVANCE_NEXT) dest = find_clip(m, k->next);
    else if (k->c.advance == CW_MUSIC_ADVANCE_RETURN && m->hold >= 0) dest = m->hold, m->next_returning = true;
    if (dest >= 0) {
      m->target = dest;
      m->advanced = true;
    }
  }
  if (m->target >= 0 && !m->scheduled && (m->cur < 0 || m->start >= 0)) schedule(m);
}

void cw__music_update(void);
void cw__music_update(void) {
  if (!pools_ready) return;
  for (int i = 0; i < mpool.slot_count; i++) {
    music **pp = cw_pool_at(&mpool, i);
    if (pp && *pp) update_one(*pp);
  }
}

bool cw_music_play(cw_music h, const char *name) {
  music *m = mget(h);
  int idx = m ? find_clip(m, name) : -1;
  if (idx < 0) {
    if (m) cw_error("music: no clip named '%s'", name ? name : "(null)");
    return false;
  }
  if (m->target == idx || (m->target < 0 && m->cur == idx && !m->scheduled)) return true;
  if (m->after == idx && m->scheduled) return true; /* already on its way through a filler */
  if (m->scheduled) { /* another switch was on its way: called off */
    cw_voice_stop(m->next_voice, 0);
    cw_voice_stop_at(m->voice, -1, 0);
    m->next_voice = (cw_voice){ 0 };
    m->scheduled = false;
  }
  m->after = -1;
  m->advanced = false;
  m->next_returning = false;
  m->target = idx;
  update_one(m);
  return true;
}
void cw_music_stop(cw_music h, float fade) {
  music *m = mget(h);
  if (!m) return;
  cw_voice_stop(m->voice, fade);
  cw_voice_stop(m->next_voice, 0);
  m->voice = m->next_voice = (cw_voice){ 0 };
  m->cur = m->target = m->after = m->hold = -1;
  m->scheduled = m->advanced = m->next_returning = false;
  m->start = -1;
}
void cw_music_set_volume(cw_music h, float volume) {
  music *m = mget(h);
  if (!m) return;
  m->d.volume = volume;
  cw_voice_set_volume(m->voice, volume);
  cw_voice_set_volume(m->next_voice, volume);
}
void cw_music_set_3d(cw_music h, const cw_voice_3d *space) {
  music *m = mget(h);
  if (!m) return;
  m->spatial = space != NULL;
  if (space) {
    m->space = *space;
    cw_voice_set_3d(m->voice, space);
    cw_voice_set_3d(m->next_voice, space);
  }
}
void cw_music_set_paused(cw_music h, bool paused) {
  music *m = mget(h);
  if (!m || m->paused == paused) return;
  m->paused = paused;
  cw_voice_set_paused(m->voice, paused);
  cw_voice_set_paused(m->next_voice, paused);
  if (!paused && m->cur >= 0) {
    /* the grid moved by the time held: beat 0 again from the voice's own clock (exact to the block) */
    clip *k = &m->clips[m->cur];
    int sr = cw_sound_source_rate(k->c.sound);
    int64_t played = cw_voice_frames_played(m->voice);
    if (played >= 0 && sr > 0) {
      m->start = cw_audio_mixed_frames() - (int64_t)floor(played * ((double)mix_rate() / sr) + 0.5);
      m->start_off = 0;
    }
  }
}
const char *cw_music_current(cw_music h) {
  music *m = mget(h);
  return m && m->cur >= 0 ? m->clips[m->cur].name : NULL;
}
const char *cw_music_pending(cw_music h) {
  music *m = mget(h);
  if (!m) return NULL;
  int t = m->scheduled ? m->next : m->target;
  return t >= 0 && t != m->cur ? m->clips[t].name : NULL;
}
int64_t cw_music_switch_frame(cw_music h) {
  music *m = mget(h);
  return m && m->scheduled ? m->at : -1;
}
cw_voice cw_music_voice(cw_music h) {
  music *m = mget(h);
  return m ? m->voice : (cw_voice){ 0 };
}
int64_t cw_music_clip_start(cw_music h) {
  music *m = mget(h);
  return m && m->cur >= 0 ? m->start : -1;
}
bool cw_music_is_playing(cw_music h) {
  music *m = mget(h);
  return m && (m->cur >= 0 || m->scheduled);
}

/* ── .cwaudio resources ─────────────────────────────────────────────── */

typedef struct res_rule { char from[48], to[48], filler[48]; cw_music_transition t; } res_rule;
struct cw_audio_stream_res {
  const char *path;
  bool interactive;
  cw_randomizer rnd;
  char initial[48];
  int nclips;
  clip clips[CW_MUSIC_MAX_CLIPS];
  int nrules;
  res_rule rules[CW_MUSIC_MAX_TRANSITIONS];
};

typedef struct kv { char key[32]; char val[256]; } kv;
/* one line's statement word and key=value pairs; false on a malformed pair */
static bool parse_line(const char *s, char *word, size_t wcap, kv *out, int *n, int cap) {
  *n = 0;
  while (*s == ' ' || *s == '\t') s++;
  size_t w = 0;
  while (*s && *s != ' ' && *s != '\t' && w + 1 < wcap) word[w++] = *s++;
  word[w] = 0;
  for (;;) {
    while (*s == ' ' || *s == '\t') s++;
    if (!*s || *s == '#') return true;
    if (*n >= cap) return false;
    kv *p = &out[(*n)++];
    size_t k = 0;
    while (*s && *s != '=' && *s != ' ' && k + 1 < sizeof p->key) p->key[k++] = *s++;
    p->key[k] = 0;
    if (*s != '=') return false;
    s++;
    size_t v = 0;
    if (*s == '"') {
      s++;
      while (*s && *s != '"' && v + 1 < sizeof p->val) p->val[v++] = *s++;
      if (*s != '"') return false;
      s++;
    } else {
      while (*s && *s != ' ' && *s != '\t' && v + 1 < sizeof p->val) p->val[v++] = *s++;
    }
    p->val[v] = 0;
  }
}
static int pick(const char *v, const char *const *names) {
  for (int i = 0; names[i]; i++)
    if (!strcmp(v, names[i])) return i;
  return -1;
}
static bool truth(const char *v) { return !strcmp(v, "true") || !strcmp(v, "1") || !strcmp(v, "yes"); }

/* a resource's sound: one per path, shared by every resource naming it (its loop points are the path's) */
static cw_map res_sounds;
static cw_sound load_sound(const char *path) {
  const char *p = cw_intern(path);
  uint64_t v;
  if (cw_map_get(&res_sounds, (uint64_t)(uintptr_t)p, &v) && cw_sound_source_rate((cw_sound){ v }) > 0) return (cw_sound){ v };
  cw_sound s = cw_sound_load(p, NULL);
  if (!CW_HANDLE_IS_NULL(s)) cw_map_set(&res_sounds, (uint64_t)(uintptr_t)p, s.id);
  return s;
}

static cw_map res_cache;
static bool res_cache_init;

static cw_audio_stream_res *parse_res(const char *path, char *text) {
  cw_audio_stream_res *r = cw_calloc(1, sizeof *r, CW_MEM_AUDIO);
  r->path = path;
  int kind = -1, lineno = 0;
  static const char *const modes[] = { "no_repeat", "any", "sequence", NULL };
  static const char *const advances[] = { "none", "next", "return", NULL };
  static const char *const ats[] = { "now", "beat", "bar", "end", NULL };
  static const char *const fades[] = { "cross", "none", "in", "out", NULL };
  char *save = NULL;
  for (char *line = strtok_r(text, "\n", &save); line; line = strtok_r(NULL, "\n", &save)) {
    lineno++;
    size_t L = strlen(line);
    while (L && (line[L - 1] == '\r' || line[L - 1] == ' ')) line[--L] = 0;
    char word[32];
    kv p[16];
    int n = 0;
    const char *first = line;
    while (*first == ' ' || *first == '\t') first++;
    if (!*first || *first == '#') continue;
    if (!parse_line(line, word, sizeof word, p, &n, 16)) goto bad;
    if (!word[0] || word[0] == '#') continue;
    if (!strcmp(word, "random") && kind < 0) {
      kind = 0;
      cw_randomizer_desc d = { 0 };
      for (int i = 0; i < n; i++) {
        if (!strcmp(p[i].key, "mode")) { int m = pick(p[i].val, modes); if (m < 0) goto bad; d.mode = (cw_random_mode)m; }
        else if (!strcmp(p[i].key, "pitch")) d.random_pitch = strtof(p[i].val, NULL);
        else if (!strcmp(p[i].key, "volume_db")) d.random_volume_db = strtof(p[i].val, NULL);
        else if (!strcmp(p[i].key, "seed")) d.seed = strtoull(p[i].val, NULL, 10);
        else goto bad;
      }
      r->rnd = cw_randomizer_new(&d);
    } else if (!strcmp(word, "sound") && kind == 0) {
      const char *sp = NULL;
      float wt = 1;
      for (int i = 0; i < n; i++) {
        if (!strcmp(p[i].key, "path")) sp = p[i].val;
        else if (!strcmp(p[i].key, "weight")) wt = strtof(p[i].val, NULL);
        else goto bad;
      }
      cw_sound s = sp ? load_sound(sp) : (cw_sound){ 0 };
      if (CW_HANDLE_IS_NULL(s) || cw_randomizer_add(r->rnd, s, wt) < 0) goto bad;
    } else if (!strcmp(word, "interactive") && kind < 0) {
      kind = 1;
      r->interactive = true;
      for (int i = 0; i < n; i++) {
        if (!strcmp(p[i].key, "initial")) snprintf(r->initial, sizeof r->initial, "%s", p[i].val);
        else goto bad;
      }
    } else if (!strcmp(word, "clip") && kind == 1 && r->nclips < CW_MUSIC_MAX_CLIPS) {
      clip *k = &r->clips[r->nclips];
      memset(k, 0, sizeof *k);
      const char *sp = NULL;
      int64_t ls = -1, le = -1;
      for (int i = 0; i < n; i++) {
        const char *key = p[i].key, *v = p[i].val;
        if (!strcmp(key, "name")) snprintf(k->name, sizeof k->name, "%s", v);
        else if (!strcmp(key, "path")) sp = v;
        else if (!strcmp(key, "bpm")) k->c.bpm = strtof(v, NULL);
        else if (!strcmp(key, "bar_beats")) k->c.bar_beats = atoi(v);
        else if (!strcmp(key, "beats")) k->c.beats = atoi(v);
        else if (!strcmp(key, "loop")) k->c.loop = truth(v);
        else if (!strcmp(key, "advance")) { int a = pick(v, advances); if (a < 0) goto bad; k->c.advance = (cw_music_advance)a; }
        else if (!strcmp(key, "next")) snprintf(k->next, sizeof k->next, "%s", v);
        else if (!strcmp(key, "loop_start")) ls = strtoll(v, NULL, 10);
        else if (!strcmp(key, "loop_end")) le = strtoll(v, NULL, 10);
        else goto bad;
      }
      if (!k->name[0] || !sp) goto bad;
      k->c.sound = load_sound(sp);
      if (CW_HANDLE_IS_NULL(k->c.sound)) goto bad;
      if (ls >= 0 || le >= 0) cw_sound_set_loop(k->c.sound, true, ls > 0 ? ls : 0, le > 0 ? le : 0); /* the sound's own points */
      r->nclips++;
    } else if (!strcmp(word, "transition") && kind == 1 && r->nrules < CW_MUSIC_MAX_TRANSITIONS) {
      res_rule *t = &r->rules[r->nrules];
      memset(t, 0, sizeof *t);
      for (int i = 0; i < n; i++) {
        const char *key = p[i].key, *v = p[i].val;
        if (!strcmp(key, "from")) snprintf(t->from, sizeof t->from, "%s", v);
        else if (!strcmp(key, "to")) snprintf(t->to, sizeof t->to, "%s", v);
        else if (!strcmp(key, "filler")) snprintf(t->filler, sizeof t->filler, "%s", v);
        else if (!strcmp(key, "at")) { int a = pick(v, ats); if (a < 0) goto bad; t->t.at = (cw_music_at)a; }
        else if (!strcmp(key, "fade")) { int f = pick(v, fades); if (f < 0) goto bad; t->t.fade = (cw_music_fade)f; }
        else if (!strcmp(key, "fade_beats")) t->t.fade_beats = strtof(v, NULL);
        else if (!strcmp(key, "same_position")) t->t.same_position = truth(v);
        else if (!strcmp(key, "hold_previous")) t->t.hold_previous = truth(v);
        else goto bad;
      }
      r->nrules++;
    } else goto bad;
  }
  if (kind < 0 || (kind == 0 && cw_randomizer_count(r->rnd) == 0) || (kind == 1 && r->nclips == 0)) {
    cw_error("audio: %s: no random or interactive statement with sounds or clips", path);
    goto fail;
  }
  if (kind == 1 && !r->initial[0]) snprintf(r->initial, sizeof r->initial, "%s", r->clips[0].name);
  return r;
bad:
  cw_error("audio: %s:%d: a statement this format does not know, or a bad value (random, sound, interactive, clip, "
           "transition: cartwheel/audio.h)", path, lineno);
fail:
  if (!CW_HANDLE_IS_NULL(r->rnd)) cw_randomizer_free(r->rnd);
  cw_free(r);
  return NULL;
}

const cw_audio_stream_res *cw_audio_stream_load(const char *asset_path) {
  if (!asset_path || cw_audio_sample_rate() <= 0) {
    cw_error("audio: cw_audio_stream_load needs cw_audio_init and a path");
    return NULL;
  }
  const char *path = cw_intern(asset_path);
  if (!res_cache_init) {
    cw_map_init(&res_cache, CW_MEM_AUDIO);
    cw_map_init(&res_sounds, CW_MEM_AUDIO);
    res_cache_init = true;
  }
  uint64_t v;
  if (cw_map_get(&res_cache, (uint64_t)(uintptr_t)path, &v)) return (const cw_audio_stream_res *)(uintptr_t)v;
  size_t len = 0;
  void *data = cw_asset_load(path, &len);
  if (!data) {
    cw_error("audio: stream resource '%s' not found", path);
    return NULL;
  }
  char *text = cw_alloc(len + 1, CW_MEM_AUDIO);
  memcpy(text, data, len);
  text[len] = 0;
  free(data);
  cw_audio_stream_res *r = parse_res(path, text);
  cw_free(text);
  if (r) cw_map_set(&res_cache, (uint64_t)(uintptr_t)path, (uint64_t)(uintptr_t)r);
  return r;
}
bool cw_audio_stream_is_interactive(const cw_audio_stream_res *res) { return res && res->interactive; }
cw_randomizer cw_audio_stream_randomizer(const cw_audio_stream_res *res) {
  return res && !res->interactive ? res->rnd : (cw_randomizer){ 0 };
}
const char *cw_audio_stream_initial(const cw_audio_stream_res *res) { return res && res->interactive ? res->initial : NULL; }
cw_music cw_audio_stream_music(const cw_audio_stream_res *res, const cw_music_desc *desc) {
  if (!res || !res->interactive) return (cw_music){ 0 };
  cw_music m = cw_music_new(desc);
  for (int i = 0; i < res->nclips; i++) {
    cw_music_clip c = res->clips[i].c;
    c.next = res->clips[i].next[0] ? res->clips[i].next : NULL;
    cw_music_add_clip(m, res->clips[i].name, &c);
  }
  for (int i = 0; i < res->nrules; i++) {
    cw_music_transition t = res->rules[i].t;
    t.filler = res->rules[i].filler[0] ? res->rules[i].filler : NULL;
    cw_music_add_transition(m, res->rules[i].from[0] ? res->rules[i].from : NULL, res->rules[i].to[0] ? res->rules[i].to : NULL, &t);
  }
  return m;
}

/* Shared between the audio module's files. Ownership of every field is
 * marked: [game] game thread only, [mix] mixer (fill callback) only,
 * [prod] stream producer (game thread or the one decode job in flight, never
 * both), [atomic] crosses threads. */
#ifndef CW_AUDIO_INTERNAL_H
#define CW_AUDIO_INTERNAL_H

#include <stdatomic.h>

#include "cartwheel/audio.h"
#include "cartwheel/core.h"
#include "cartwheel/jobs.h"
#include "cartwheel/math.h"
#include "miniaudio.h"

/* ── decoders (decode.c) ─────────────────────────────────────────────── */

typedef enum { CW__DEC_NONE, CW__DEC_MA, CW__DEC_VORBIS, CW__DEC_OPUS, CW__DEC_XMP } cw__dec_kind;

typedef struct cw__decoder {
  cw__dec_kind kind;
  int channels;   /* output channels: 1 or 2 */
  int rate;
  int64_t length; /* frames; -1 = unknown */
  void *impl;
  const uint8_t *data; /* not owned */
  size_t len;
} cw__decoder;

cw__dec_kind cw__dec_detect(const void *data, size_t len);
bool cw__dec_open(cw__decoder *d, const void *data, size_t len); /* allocates */
/* Interleaved float, d->channels per frame. Returns frames; 0 = end. */
int cw__dec_read(cw__decoder *d, float *out, int frames);
bool cw__dec_seek(cw__decoder *d, int64_t frame);
void cw__dec_close(cw__decoder *d);

/* ── SPSC queue (fixed-size elements, capacity a power of two) ──────── */

typedef struct cw__spsc {
  uint8_t *data;
  uint32_t elem, mask;
  _Atomic uint32_t head; /* consumer */
  _Atomic uint32_t tail; /* producer */
} cw__spsc;

void cw__spsc_init(cw__spsc *q, uint32_t elem, uint32_t capacity);
void cw__spsc_free(cw__spsc *q);
bool cw__spsc_push(cw__spsc *q, const void *e);
bool cw__spsc_pop(cw__spsc *q, void *e);
uint32_t cw__spsc_free_slots(cw__spsc *q); /* as seen by the producer */

/* ── streams ─────────────────────────────────────────────────────────── */

typedef struct cw__stream {
  cw__decoder dec;         /* [prod] */
  bool loop;               /* [prod] */
  int channels;            /* ring frame width (1|2), fixed while in use */
  float *ring;             /* cap * channels floats, allocated at init */
  uint32_t cap;            /* frames, power of two */
  _Atomic uint64_t write;  /* [prod] frames written */
  _Atomic uint64_t read;   /* [mix] frames the mixer no longer needs */
  _Atomic uint64_t end;    /* [prod] total frames once the decoder finished; UINT64_MAX before */
  _Atomic int job_busy;    /* [prod] a decode job is running */
  cw_job *job;             /* [game] */
  bool in_use;             /* [game] */
  int voice_slot;          /* [game] */
} cw__stream;

/* Decode into the ring until it is full or the source ended. [prod] */
void cw__stream_fill(cw__stream *st);

/* ── effects (fx.c) ──────────────────────────────────────────────────── */

#define CW__RV_COMBS 8
#define CW__RV_ALLPASSES 4
typedef struct cw__fx {
  cw_fx_desc desc;   /* [mix] after add */
  bool bypass;       /* [mix] */
  int rate;
  ma_biquad bq;      /* filters */
  ma_delay delay;    /* delay */
  struct {           /* Freeverb */
    float *comb[2][CW__RV_COMBS], comb_store[2][CW__RV_COMBS];
    int comb_len[2][CW__RV_COMBS], comb_pos[2][CW__RV_COMBS];
    float *ap[2][CW__RV_ALLPASSES];
    int ap_len[2][CW__RV_ALLPASSES], ap_pos[2][CW__RV_ALLPASSES];
    float feedback, damp1, damp2, wet1, wet2, dry, gain;
  } rv;
  struct { float env, att, rel, thr, slope, makeup, ceil; } dyn; /* compressor + limiter */
} cw__fx;

cw__fx *cw__fx_create(const cw_fx_desc *d, int rate); /* [game]: allocates; NULL on bad desc */
void cw__fx_destroy(cw__fx *fx);                      /* [game] */
void cw__fx_set(cw__fx *fx, const cw_fx_desc *d);     /* [mix]: no allocation */
void cw__fx_process(cw__fx *fx, float *buf, int frames); /* [mix] stereo, in place */

/* ── commands (game -> mixer) and events (mixer -> game) ─────────────── */

typedef enum {
  CW__CMD_PLAY, CW__CMD_STOP, CW__CMD_PAUSE, CW__CMD_VOLUME, CW__CMD_PITCH, CW__CMD_PAN, CW__CMD_BUS,
  CW__CMD_FADE, CW__CMD_3D, CW__CMD_POSITION, CW__CMD_OCCLUSION, CW__CMD_LISTENER, CW__CMD_SPEED_OF_SOUND,
  CW__CMD_BUS_SETUP, CW__CMD_BUS_VOLUME, CW__CMD_BUS_AUDIBLE, CW__CMD_BUS_SEND, CW__CMD_FX_ADD,
  CW__CMD_FX_SET, CW__CMD_FX_BYPASS, CW__CMD_FX_REMOVE,
} cw__cmd_type;

typedef struct cw__play {
  uint64_t sound_id;
  const float *pcm; /* sample data, or NULL for a stream */
  int64_t frames;
  int channels;
  int stream;       /* stream slot, -1 for samples */
  float ratio;      /* source rate / mix rate */
  cw_play_desc d;   /* resolved (no zero defaults left) */
} cw__play;

typedef struct cw__cmd {
  uint8_t type;
  int16_t slot;  /* voice slot or bus index */
  uint32_t gen;  /* voice generation */
  union {
    cw__play play;
    struct { float a, b; bool flag; } f;
    struct { t3_vec3 p, v, fwd, up; int index, count; } listener;
    struct { t3_vec3 p, v; } pos;
    cw_voice_3d space;
    struct { int parent; } bus;
    struct { int slot, target; float level; } send;
    struct { int slot; cw__fx *fx; cw_fx_desc desc; bool bypass; } fx;
  };
} cw__cmd;

typedef enum { CW__EV_VOICE_ENDED, CW__EV_FX_FREED } cw__ev_type;
typedef struct cw__event {
  uint8_t type;
  int16_t slot;
  int16_t stream;
  uint32_t gen;
  uint64_t sound_id;
  cw__fx *fx;
} cw__event;

/* ── the mixer (mixer.c) ─────────────────────────────────────────────── */

enum { CW__V_FREE, CW__V_WAITING, CW__V_PLAYING, CW__V_ZOMBIE };

typedef struct cw__mvoice {
  uint8_t state;
  uint32_t gen;
  uint64_t sound_id;
  const float *pcm;
  int64_t frames;
  int channels;
  int stream;
  uint64_t ipos;     /* integer source position */
  uint32_t frac;     /* 0.32 fraction */
  float ratio;       /* source rate / mix rate */
  float volume, pitch, pan;
  bool loop, paused, priming;
  int64_t loop_start, loop_end;
  float fade, fade_target, fade_step; /* fade_step per frame, >= 0 */
  bool stop_at_fade_end;
  int bus;
  bool spatial;
  cw_voice_3d sp;
  float occlusion, lp[2];
  float gl, gr;      /* gains applied at the end of the last block */
  bool gains_valid;
  int64_t start_frame;
  uint64_t played;   /* the music clock */
} cw__mvoice;

typedef struct cw__mbus {
  bool used, audible;
  int parent;
  float volume, cur_volume;
  struct { int target; float level, cur; } sends[CW_AUDIO_MAX_SENDS];
  cw__fx *fx[CW_AUDIO_MAX_FX];
  int nfx;
  float *buf; /* CW_AUDIO_BLOCK * 2 */
} cw__mbus;

typedef struct cw__mixer {
  int rate;
  int max_voices;
  cw__mvoice *voices;
  cw__mbus buses[CW_AUDIO_MAX_BUSES];
  float *scratch;          /* CW_AUDIO_BLOCK * 2 */
  /* the listeners (splitscreen: one per view); listener 0 is cw_audio_set_listener's */
  t3_vec3 lpos[CW_AUDIO_MAX_LISTENERS], lvel[CW_AUDIO_MAX_LISTENERS], lfwd[CW_AUDIO_MAX_LISTENERS], lup[CW_AUDIO_MAX_LISTENERS];
  int nlisteners;
  float speed_of_sound;
  uint64_t mixed;          /* frames */
  /* published for the game thread */
  _Atomic uint64_t pub_mixed;
  _Atomic uint32_t pub_blocks, pub_underruns, pub_underrun_frames, pub_peak_bits;
  _Atomic int32_t pub_voices, pub_streams;
  _Atomic uint32_t *clock_gen; /* per voice slot: generation the clock belongs to */
  _Atomic uint64_t *clock;     /* per voice slot: frames played */
} cw__mixer;

/* ── the whole system ────────────────────────────────────────────────── */

typedef struct cw__sound {
  bool stream, released;
  float *pcm;          /* samples: interleaved at the mix rate */
  int64_t frames;      /* samples: frames; streams: decoder length or -1 */
  int channels, rate;  /* rate: of the frames as played (mix rate for samples) */
  void *data;          /* streams: encoded bytes */
  size_t len;
  int max_voices, priority;
  float volume;
  cw_bus bus;
  int live;            /* voices playing it (logical) */
  int refs;            /* voices the mixer may still read it through */
} cw__sound;

typedef struct cw__gvoice {
  uint32_t gen;        /* odd = live */
  bool live;
  uint64_t sound_id;
  int priority;
  uint64_t order;      /* start order: lower = older */
} cw__gvoice;

typedef struct cw__gbus {
  const char *name;    /* interned */
  bool used, mute, solo;
  int parent;
  int nfx;
} cw__gbus;

typedef struct cw__audio {
  bool ready, offline;
  cw_audio_desc desc;
  cw__mixer mix;
  cw__spsc cmds, events;
  cw_pool sounds;
  cw__gvoice *gvoices;
  uint64_t play_order;
  cw__stream *streams;
  int max_streams;
  cw__gbus buses[CW_AUDIO_MAX_BUSES];
  uint32_t dropped;
  /* [game] set while no mix runs; [mix] read: a mix applies no command at or
   * past cmd_fence while fenced (cw__audio_fence_hook) */
  bool fenced;
  uint32_t cmd_fence;
  cw_audio_stats stats;
  /* debug-field mirrors */
  uint32_t dbg_mixed, dbg_underruns, dbg_dropped;
  int32_t dbg_voices;
} cw__audio;

extern cw__audio cw__au;

void cw__mixer_init(cw__mixer *m, int rate, int max_voices);
void cw__mixer_free(cw__mixer *m);
/* The fill callback body: drains commands, mixes `frames` stereo frames. */
void cw__mixer_mix(float *out, int frames);
/* Offline: called before each block so streams hold enough data. */
void cw__offline_prepare_streams(void);

#endif

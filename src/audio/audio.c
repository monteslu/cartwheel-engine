/* The game-thread side of the audio module: sounds, voice allocation
 * (limits, priorities, stealing), buses, the command queue, mixer events,
 * stream decode jobs and offline rendering. The mixer itself is mixer.c. */
#include <stdlib.h>
#include <string.h>

#include "internal.h"
#include "cartwheel/debug.h"
#include "cartwheel/platform.h"

cw__audio cw__au;

/* ── SPSC queue ─────────────────────────────────────────────────────── */

void cw__spsc_init(cw__spsc *q, uint32_t elem, uint32_t capacity) {
  uint32_t cap = 1;
  while (cap < capacity) cap <<= 1;
  q->data = cw_calloc(cap, elem, CW_MEM_AUDIO);
  q->elem = elem;
  q->mask = cap - 1;
  atomic_store(&q->head, 0);
  atomic_store(&q->tail, 0);
}
void cw__spsc_free(cw__spsc *q) {
  cw_free(q->data);
  q->data = NULL;
}
bool cw__spsc_push(cw__spsc *q, const void *e) {
  uint32_t t = atomic_load_explicit(&q->tail, memory_order_relaxed);
  uint32_t h = atomic_load_explicit(&q->head, memory_order_acquire);
  if (t - h > q->mask) return false;
  memcpy(q->data + (size_t)(t & q->mask) * q->elem, e, q->elem);
  atomic_store_explicit(&q->tail, t + 1, memory_order_release);
  return true;
}
bool cw__spsc_pop(cw__spsc *q, void *e) {
  uint32_t h = atomic_load_explicit(&q->head, memory_order_relaxed);
  uint32_t t = atomic_load_explicit(&q->tail, memory_order_acquire);
  if (h == t) return false;
  memcpy(e, q->data + (size_t)(h & q->mask) * q->elem, q->elem);
  atomic_store_explicit(&q->head, h + 1, memory_order_release);
  return true;
}
uint32_t cw__spsc_free_slots(cw__spsc *q) {
  uint32_t t = atomic_load_explicit(&q->tail, memory_order_relaxed);
  uint32_t h = atomic_load_explicit(&q->head, memory_order_acquire);
  return q->mask + 1 - (t - h);
}

static void send(const cw__cmd *c) {
  if (!cw__au.ready) return;
  if (!cw__spsc_push(&cw__au.cmds, c)) {
    if (cw__au.dropped++ == 0)
      cw_warn("audio: command queue full (%u): call cw_audio_update() every frame, or raise "
              "cw_audio_desc.command_capacity", cw__au.cmds.mask + 1);
  }
}

/* ── streams ────────────────────────────────────────────────────────── */

void cw__stream_fill(cw__stream *st) {
  if (atomic_load_explicit(&st->end, memory_order_relaxed) != UINT64_MAX) return;
  uint64_t w = atomic_load_explicit(&st->write, memory_order_relaxed);
  bool restarted = false;
  for (;;) {
    uint64_t r = atomic_load_explicit(&st->read, memory_order_acquire);
    uint64_t space = st->cap - (w - r);
    if (space == 0) break;
    uint32_t at = (uint32_t)(w & (st->cap - 1));
    int n = (int)CW_MIN(space, (uint64_t)(st->cap - at));
    n = CW_MIN(n, 4096);
    int got = cw__dec_read(&st->dec, st->ring + (size_t)at * st->channels, n);
    if (got > 0) {
      w += (uint64_t)got;
      atomic_store_explicit(&st->write, w, memory_order_release);
      restarted = false;
      continue;
    }
    /* the source ended */
    if (st->loop && !restarted && cw__dec_seek(&st->dec, 0)) {
      restarted = true; /* a second empty read right after a restart = empty source */
      continue;
    }
    atomic_store_explicit(&st->end, w, memory_order_release);
    break;
  }
}

static void stream_job(void *ctx) {
  cw__stream *st = ctx;
  cw__stream_fill(st);
  atomic_store_explicit(&st->job_busy, 0, memory_order_release);
}

static void stream_wait(cw__stream *st) {
  if (st->job) {
    cw_job_wait(st->job);
    st->job = NULL;
  }
}

static void stream_release(int i) {
  cw__stream *st = &cw__au.streams[i];
  stream_wait(st);
  cw__dec_close(&st->dec);
  st->in_use = false;
  st->voice_slot = -1;
}

/* Kick decode jobs where rings have room (game thread, every update). */
static void pump_streams(void) {
  bool inline_decode = cw_jobs_thread_count() <= 1;
  for (int i = 0; i < cw__au.max_streams; i++) {
    cw__stream *st = &cw__au.streams[i];
    if (!st->in_use) continue;
    uint64_t used = atomic_load(&st->write) - atomic_load(&st->read);
    /* finished jobs are collected; a job still out while the ring is nearly
     * dry is waited for (cw_job_wait runs it right here if no worker has
     * picked it up: workers may be busy, or in a cart not started yet) */
    if (st->job && (!atomic_load_explicit(&st->job_busy, memory_order_acquire) || used < st->cap / 8)) stream_wait(st);
    if (st->job || atomic_load_explicit(&st->end, memory_order_relaxed) != UINT64_MAX) continue;
    used = atomic_load(&st->write) - atomic_load(&st->read);
    if (st->cap - used < st->cap / 4) continue;
    if (inline_decode) cw__stream_fill(st);
    else {
      atomic_store(&st->job_busy, 1);
      st->job = cw_job_single(stream_job, st);
    }
  }
}

/* Offline: before every block, refill (on the job pool, all streams in
 * parallel) any ring below 3/4 full and wait, so a block never finds a ring
 * short: the output cannot depend on timing. */
void cw__offline_prepare_streams(void) {
  for (int i = 0; i < cw__au.max_streams; i++) {
    cw__stream *st = &cw__au.streams[i];
    if (!st->in_use || atomic_load(&st->end) != UINT64_MAX) continue;
    uint64_t used = atomic_load(&st->write) - atomic_load(&st->read);
    if (st->cap - used < st->cap / 4) continue;
    atomic_store(&st->job_busy, 1);
    st->job = cw_job_single(stream_job, st);
  }
  for (int i = 0; i < cw__au.max_streams; i++) stream_wait(&cw__au.streams[i]);
}

/* ── sounds ─────────────────────────────────────────────────────────── */

static cw__sound *sound_get(cw_sound s) {
  cw__sound *snd = cw__au.ready ? cw_pool_get(&cw__au.sounds, s.id) : NULL;
  return snd && !snd->released ? snd : NULL;
}

static void sound_destroy(uint64_t id) {
  cw__sound *snd = cw_pool_get(&cw__au.sounds, id);
  if (!snd) return;
  cw_free(snd->pcm);
  cw_free(snd->data);
  cw_pool_release(&cw__au.sounds, id);
}

static cw__sound *sound_new(const cw_sound_desc *desc, uint64_t *id) {
  cw__sound *snd = cw_pool_alloc(&cw__au.sounds, id);
  cw_sound_desc d = desc ? *desc : (cw_sound_desc){ 0 };
  snd->max_voices = d.max_voices;
  snd->priority = d.priority;
  snd->volume = d.volume > 0 ? d.volume : 1.0f;
  snd->bus = d.bus;
  return snd;
}

/* Resample interleaved PCM to the mix rate with miniaudio's linear
 * resampler (with its low-pass). Returns a new cw_alloc'd buffer. */
static float *resample(const float *in, int64_t frames, int ch, int from, int to, int64_t *out_frames) {
  ma_resampler_config cfg = ma_resampler_config_init(ma_format_f32, (ma_uint32)ch, (ma_uint32)from, (ma_uint32)to,
                                                     ma_resample_algorithm_linear);
  ma_resampler rs;
  if (ma_resampler_init(&cfg, NULL, &rs) != MA_SUCCESS) return NULL;
  int64_t want = (int64_t)(((uint64_t)frames * (uint64_t)to + (uint64_t)from - 1) / (uint64_t)from);
  float *out = cw_alloc((size_t)want * ch * sizeof(float), CW_MEM_AUDIO);
  int64_t in_pos = 0, out_pos = 0;
  float zeros[256 * 2] = { 0 };
  while (out_pos < want) {
    ma_uint64 fin, fout = (ma_uint64)(want - out_pos);
    const float *src;
    if (in_pos < frames) {
      src = in + in_pos * ch;
      fin = (ma_uint64)(frames - in_pos);
    } else { /* flush the filter's latency with silence */
      src = zeros;
      fin = 256;
    }
    if (ma_resampler_process_pcm_frames(&rs, src, &fin, out + out_pos * ch, &fout) != MA_SUCCESS) break;
    if (in_pos < frames) in_pos += (int64_t)fin;
    out_pos += (int64_t)fout;
    if (fin == 0 && fout == 0) break;
  }
  ma_resampler_uninit(&rs, NULL);
  if (out_pos < want) memset(out + out_pos * ch, 0, (size_t)(want - out_pos) * ch * sizeof(float));
  *out_frames = want;
  return out;
}

static bool make_sample(cw__sound *snd, float *pcm, int64_t frames, int ch, int rate) {
  int mix = cw__au.mix.rate;
  if (rate != mix) {
    int64_t n = 0;
    float *r = resample(pcm, frames, ch, rate, mix, &n);
    cw_free(pcm);
    if (!r) return false;
    pcm = r;
    frames = n;
  }
  snd->pcm = pcm;
  snd->frames = frames;
  snd->channels = ch;
  snd->rate = mix;
  return true;
}

/* Whole-file decode with a decoder already open. */
static int64_t decode_all(cw__decoder *dec, float **pcm) {
  int64_t cap = dec->length > 0 ? dec->length : 48000, n = 0;
  float *buf = cw_alloc((size_t)cap * dec->channels * sizeof(float), CW_MEM_AUDIO);
  for (;;) {
    if (n == cap) {
      if (dec->kind == CW__DEC_XMP && n >= (int64_t)dec->rate * 600) break; /* modules: 10 minutes max */
      cap *= 2;
      buf = cw_realloc(buf, (size_t)cap * dec->channels * sizeof(float), CW_MEM_AUDIO);
    }
    int got = cw__dec_read(dec, buf + n * dec->channels, (int)CW_MIN(cap - n, (int64_t)8192));
    if (got <= 0) break;
    n += got;
  }
  *pcm = buf;
  return n;
}

cw_sound cw_sound_load_memory(const void *data, size_t len, const cw_sound_desc *desc) {
  if (!cw__au.ready) {
    cw_error("audio: cw_sound_load before cw_audio_init");
    return (cw_sound){ 0 };
  }
  cw__decoder dec;
  void *copy = cw_alloc(len, CW_MEM_AUDIO);
  memcpy(copy, data, len);
  if (!cw__dec_open(&dec, copy, len)) {
    cw_free(copy);
    cw_error("audio: data is not a WAV/FLAC/MP3/Ogg Vorbis/Ogg Opus/tracker module this build can decode");
    return (cw_sound){ 0 };
  }
  cw_sound_mode mode = desc ? desc->mode : CW_SOUND_AUTO;
  if (dec.kind == CW__DEC_XMP) mode = CW_SOUND_STREAM;
  if (mode == CW_SOUND_AUTO)
    mode = dec.length >= 0 && dec.length * dec.channels * 4 <= 2 * 1024 * 1024 ? CW_SOUND_SAMPLE : CW_SOUND_STREAM;
  uint64_t id;
  cw__sound *snd = sound_new(desc, &id);
  if (mode == CW_SOUND_STREAM) {
    snd->stream = true;
    snd->data = copy;
    snd->len = len;
    snd->frames = dec.length;
    snd->channels = dec.channels;
    snd->rate = dec.rate;
    cw__dec_close(&dec);
  } else {
    float *pcm;
    int ch = dec.channels, rate = dec.rate;
    int64_t n = decode_all(&dec, &pcm);
    cw__dec_close(&dec);
    cw_free(copy);
    if (n <= 0 || !make_sample(snd, pcm, n, ch, rate)) {
      if (n <= 0) cw_free(pcm);
      cw_pool_release(&cw__au.sounds, id);
      cw_error("audio: decoded no audio");
      return (cw_sound){ 0 };
    }
  }
  return (cw_sound){ id };
}

cw_sound cw_sound_load(const char *path, const cw_sound_desc *desc) {
  size_t len = 0;
  void *data = cw_asset_load(path, &len);
  if (!data) {
    cw_error("audio: asset '%s' not found (natively: is it under CW_ASSETS / ./assets? carts: packed with "
             "cw_add_app(... ASSETS dir)?)", path);
    return (cw_sound){ 0 };
  }
  cw_sound s = cw_sound_load_memory(data, len, desc);
  free(data);
  if (CW_HANDLE_IS_NULL(s)) cw_error("audio: could not load '%s'", path);
  return s;
}

cw_sound cw_sound_from_pcm(const float *pcm, int frames, int channels, int rate, const cw_sound_desc *desc) {
  if (!cw__au.ready || !pcm || frames <= 0 || channels < 1 || channels > 2 || rate <= 0) {
    cw_error("audio: cw_sound_from_pcm needs cw_audio_init, frames > 0, 1 or 2 channels and a rate");
    return (cw_sound){ 0 };
  }
  float *copy = cw_alloc((size_t)frames * channels * sizeof(float), CW_MEM_AUDIO);
  memcpy(copy, pcm, (size_t)frames * channels * sizeof(float));
  uint64_t id;
  cw__sound *snd = sound_new(desc, &id);
  if (!make_sample(snd, copy, frames, channels, rate)) {
    cw_pool_release(&cw__au.sounds, id);
    return (cw_sound){ 0 };
  }
  return (cw_sound){ id };
}

static void stop_slot(int slot, float fade);

void cw_sound_free(cw_sound s) {
  cw__sound *snd = sound_get(s);
  if (!snd) return;
  snd->released = true;
  for (int i = 0; i < cw__au.mix.max_voices; i++)
    if (cw__au.gvoices[i].live && cw__au.gvoices[i].sound_id == s.id) stop_slot(i, 0);
  if (snd->refs == 0) sound_destroy(s.id);
}

int64_t cw_sound_length_frames(cw_sound s) {
  cw__sound *snd = sound_get(s);
  return snd ? snd->frames : -1;
}
int cw_sound_source_rate(cw_sound s) {
  cw__sound *snd = sound_get(s);
  return snd ? snd->rate : 0;
}
bool cw_sound_is_stream(cw_sound s) {
  cw__sound *snd = sound_get(s);
  return snd && snd->stream;
}

/* ── voices ─────────────────────────────────────────────────────────── */

static int voice_slot(cw_voice v) {
  if (!cw__au.ready || CW_HANDLE_IS_NULL(v)) return -1;
  uint32_t i = CW_HANDLE_INDEX(v);
  if (i >= (uint32_t)cw__au.mix.max_voices) return -1;
  cw__gvoice *g = &cw__au.gvoices[i];
  return g->live && g->gen == CW_HANDLE_GEN(v) ? (int)i : -1;
}

static void stop_slot(int slot, float fade) {
  cw__gvoice *g = &cw__au.gvoices[slot];
  cw__cmd c = { .type = CW__CMD_STOP, .slot = (int16_t)slot, .gen = g->gen };
  c.f.a = fade;
  send(&c);
}

/* The slot a new voice of `snd` at `priority` goes into, or -1. */
static int pick_slot(cw__sound *snd, uint64_t sound_id, int priority) {
  cw__gvoice *gv = cw__au.gvoices;
  int n = cw__au.mix.max_voices;
  if (snd->max_voices > 0 && snd->live >= snd->max_voices) { /* this sound's oldest */
    int best = -1;
    for (int i = 0; i < n; i++)
      if (gv[i].live && gv[i].sound_id == sound_id && (best < 0 || gv[i].order < gv[best].order)) best = i;
    return best;
  }
  for (int i = 0; i < n; i++)
    if (!gv[i].live) return i;
  int best = -1;
  for (int i = 0; i < n; i++) {
    if (!gv[i].live || gv[i].priority > priority) continue;
    if (best < 0 || gv[i].priority < gv[best].priority ||
        (gv[i].priority == gv[best].priority && gv[i].order < gv[best].order))
      best = i;
  }
  return best;
}

static cw__sound *sound_by_id(uint64_t id) { return cw_pool_get(&cw__au.sounds, id); }

cw_voice cw_audio_play(cw_sound s, const cw_play_desc *desc) {
  cw__sound *snd = sound_get(s);
  if (!snd) return (cw_voice){ 0 };
  cw_play_desc d = desc ? *desc : (cw_play_desc){ 0 };
  if (d.volume <= 0) d.volume = snd->volume;
  if (d.pitch <= 0) d.pitch = 1.0f;
  if (d.bus <= 0) d.bus = snd->bus;
  if (d.priority == 0) d.priority = snd->priority;
  if (d.offset < 0) d.offset = 0;
  int stream = -1;
  if (snd->stream) {
    for (int i = 0; i < cw__au.max_streams; i++)
      if (!cw__au.streams[i].in_use) { stream = i; break; }
    if (stream < 0) {
      cw_warn("audio: all %d stream slots busy (cw_audio_desc.max_streams)", cw__au.max_streams);
      return (cw_voice){ 0 };
    }
  }
  int slot = pick_slot(snd, s.id, d.priority);
  if (slot < 0) return (cw_voice){ 0 };
  cw__gvoice *g = &cw__au.gvoices[slot];
  if (g->live) { /* steal: the old voice's memory refs drop when the mixer reports it */
    cw__sound *old = sound_by_id(g->sound_id);
    if (old) old->live--;
  }
  if (stream >= 0) {
    cw__stream *st = &cw__au.streams[stream];
    if (!cw__dec_open(&st->dec, snd->data, snd->len)) return (cw_voice){ 0 };
    if (d.offset > 0 && !cw__dec_seek(&st->dec, d.offset)) cw_warn("audio: stream could not seek to %lld", (long long)d.offset);
    st->in_use = true;
    st->voice_slot = slot;
    st->loop = d.loop;
    st->channels = st->dec.channels;
    atomic_store(&st->write, 0);
    atomic_store(&st->read, 0);
    atomic_store(&st->end, UINT64_MAX);
    if (!cw__au.offline) pump_streams(); /* start decoding now */
  }
  g->gen = (g->gen + 2) | 1;
  g->live = true;
  g->sound_id = s.id;
  g->priority = d.priority;
  g->order = ++cw__au.play_order;
  snd->live++;
  snd->refs++;
  cw__cmd c = { .type = CW__CMD_PLAY, .slot = (int16_t)slot, .gen = g->gen };
  c.play.sound_id = s.id;
  c.play.pcm = snd->pcm;
  c.play.frames = snd->frames;
  c.play.channels = snd->channels;
  c.play.stream = stream;
  c.play.ratio = (float)snd->rate / (float)cw__au.mix.rate;
  c.play.d = d;
  send(&c);
  return CW_HANDLE_MAKE(cw_voice, slot, g->gen);
}

static void voice_cmd(cw_voice v, cw__cmd_type type, float a, float b, bool flag) {
  int slot = voice_slot(v);
  if (slot < 0) return;
  cw__cmd c = { .type = (uint8_t)type, .slot = (int16_t)slot, .gen = cw__au.gvoices[slot].gen };
  c.f.a = a;
  c.f.b = b;
  c.f.flag = flag;
  send(&c);
}

void cw_voice_stop(cw_voice v, float fade) { voice_cmd(v, CW__CMD_STOP, fade, 0, false); }
void cw_voice_set_paused(cw_voice v, bool paused) { voice_cmd(v, CW__CMD_PAUSE, 0, 0, paused); }
void cw_voice_set_volume(cw_voice v, float volume) { voice_cmd(v, CW__CMD_VOLUME, volume, 0, false); }
void cw_voice_set_pitch(cw_voice v, float pitch) { voice_cmd(v, CW__CMD_PITCH, pitch > 0 ? pitch : 1.0f, 0, false); }
void cw_voice_set_pan(cw_voice v, float pan) { voice_cmd(v, CW__CMD_PAN, cw_clampf(pan, -1, 1), 0, false); }
void cw_voice_set_bus(cw_voice v, cw_bus bus) { voice_cmd(v, CW__CMD_BUS, (float)bus, 0, false); }
void cw_voice_fade(cw_voice v, float target, float seconds) { voice_cmd(v, CW__CMD_FADE, target, seconds, false); }
void cw_voice_set_occlusion(cw_voice v, float amount) { voice_cmd(v, CW__CMD_OCCLUSION, amount, 0, false); }

void cw_voice_set_3d(cw_voice v, const cw_voice_3d *space) {
  int slot = voice_slot(v);
  if (slot < 0 || !space) return;
  cw__cmd c = { .type = CW__CMD_3D, .slot = (int16_t)slot, .gen = cw__au.gvoices[slot].gen };
  c.space = *space;
  send(&c);
}

void cw_voice_set_position(cw_voice v, t3_vec3 position, t3_vec3 velocity) {
  int slot = voice_slot(v);
  if (slot < 0) return;
  cw__cmd c = { .type = CW__CMD_POSITION, .slot = (int16_t)slot, .gen = cw__au.gvoices[slot].gen };
  c.pos.p = position;
  c.pos.v = velocity;
  send(&c);
}

bool cw_voice_is_playing(cw_voice v) { return voice_slot(v) >= 0; }

int64_t cw_voice_frames_played(cw_voice v) {
  int slot = voice_slot(v);
  if (slot < 0) return -1;
  cw__mixer *m = &cw__au.mix;
  for (int tries = 0; tries < 4; tries++) {
    uint32_t g1 = atomic_load_explicit(&m->clock_gen[slot], memory_order_acquire);
    uint64_t val = atomic_load_explicit(&m->clock[slot], memory_order_acquire);
    uint32_t g2 = atomic_load_explicit(&m->clock_gen[slot], memory_order_acquire);
    if (g1 != g2) continue;
    return g1 == cw__au.gvoices[slot].gen ? (int64_t)val : 0; /* not started in the mixer yet */
  }
  return 0;
}

int64_t cw_audio_mixed_frames(void) {
  return cw__au.ready ? (int64_t)atomic_load_explicit(&cw__au.mix.pub_mixed, memory_order_acquire) : 0;
}

/* ── listener ───────────────────────────────────────────────────────── */

void cw_audio_set_listener(t3_vec3 position, t3_vec3 velocity, t3_vec3 forward, t3_vec3 up) {
  cw_audio_set_listener_at(0, position, velocity, forward, up);
}

void cw_audio_set_listener_at(int index, t3_vec3 position, t3_vec3 velocity, t3_vec3 forward, t3_vec3 up) {
  if (index < 0 || index >= CW_AUDIO_MAX_LISTENERS) return;
  cw__cmd c = { .type = CW__CMD_LISTENER };
  c.listener.index = index;
  c.listener.p = position;
  c.listener.v = velocity;
  c.listener.fwd = forward;
  c.listener.up = up;
  send(&c);
}

void cw_audio_set_listener_count(int n) {
  cw__cmd c = { .type = CW__CMD_LISTENER };
  c.listener.index = -1;
  c.listener.count = CW_CLAMP(n, 1, CW_AUDIO_MAX_LISTENERS);
  send(&c);
}

void cw_audio_set_speed_of_sound(float units_per_second) {
  cw__cmd c = { .type = CW__CMD_SPEED_OF_SOUND };
  c.f.a = units_per_second;
  send(&c);
}

/* ── output mode and HRTF ───────────────────────────────────────────── */

static bool send_hrtf(cw__hrtf *h) {
  cw__cmd c = { .type = CW__CMD_HRTF };
  c.hrtf = h;
  if (!cw__spsc_push(&cw__au.cmds, &c)) { /* never leak the table */
    cw__au.dropped++;
    cw__hrtf_free(h);
    return false;
  }
  cw__au.hrtf_loaded = true;
  return true;
}

bool cw_audio_load_hrtf(const char *asset_path) {
  if (!cw__au.ready) {
    cw_error("audio: cw_audio_load_hrtf before cw_audio_init");
    return false;
  }
  cw__hrtf *h;
  if (!asset_path) {
    size_t len;
    const void *data = cw__hrtf_builtin(&len);
    h = cw__hrtf_parse(data, len, cw__au.mix.rate);
  } else {
    size_t len;
    void *data = cw_asset_load(asset_path, &len);
    if (!data) {
      cw_error("audio: HRTF table '%s' not found", asset_path);
      return false;
    }
    h = cw__hrtf_parse(data, len, cw__au.mix.rate);
    free(data);
    if (!h) cw_error("audio: '%s' is not a usable .cwhrtf table; keeping the current one", asset_path);
  }
  return h && send_hrtf(h);
}

void cw_audio_set_output(cw_audio_output output) {
  if (!cw__au.ready) return;
  if (output == CW_AUDIO_HEADPHONES && !cw__au.hrtf_loaded) cw_audio_load_hrtf(NULL);
  cw__au.output = output == CW_AUDIO_HEADPHONES ? CW_AUDIO_HEADPHONES : CW_AUDIO_SPEAKERS;
  cw__cmd c = { .type = CW__CMD_OUTPUT };
  c.ival = cw__au.output;
  send(&c);
}

cw_audio_output cw_audio_get_output(void) { return (cw_audio_output)cw__au.output; }

void cw_audio_set_hrtf_voice_limit(int voices) {
  if (!cw__au.ready) return;
  cw__au.hrtf_limit = CW_CLAMP(voices, 0, cw__au.desc.max_voices);
  cw__cmd c = { .type = CW__CMD_HRTF_LIMIT };
  c.ival = cw__au.hrtf_limit;
  send(&c);
}

/* ── reverb zones ───────────────────────────────────────────────────── */

static bool zone_ok(cw_reverb_zone z) { return cw__au.ready && z >= 0 && z < CW_AUDIO_MAX_REVERB_ZONES && cw__au.zone_used[z]; }

cw_reverb_zone cw_audio_add_reverb_zone(const cw_reverb_zone_desc *desc) {
  if (!cw__au.ready || !desc) return -1;
  int z = 0;
  while (z < CW_AUDIO_MAX_REVERB_ZONES && cw__au.zone_used[z]) z++;
  if (z == CW_AUDIO_MAX_REVERB_ZONES) {
    cw_error("audio: all %d reverb zones are in use (cw_audio_remove_reverb_zone one first)", CW_AUDIO_MAX_REVERB_ZONES);
    return -1;
  }
  cw_reverb_zone_desc d = *desc;
  if (d.decay_s <= 0) d.decay_s = 1.5f;
  cw_fx_desc fd = { .type = CW_FX_FDN_REVERB, .fdn = { d.decay_s, d.hf_damp_hz, d.predelay_s, d.size, 1, 0 } };
  cw__fx *fx = cw__fx_create(&fd, cw__au.mix.rate);
  if (!fx) return -1;
  cw__cmd c = { .type = CW__CMD_ZONE_ADD, .slot = (int16_t)z };
  c.zone.fx = fx;
  c.zone.desc = d;
  if (!cw__spsc_push(&cw__au.cmds, &c)) {
    cw__au.dropped++;
    cw__fx_destroy(fx);
    return -1;
  }
  cw__au.zone_used[z] = true;
  return z;
}

void cw_audio_set_reverb_zone(cw_reverb_zone zone, const cw_reverb_zone_desc *desc) {
  if (!zone_ok(zone) || !desc) return;
  cw__cmd c = { .type = CW__CMD_ZONE_SET, .slot = (int16_t)zone };
  c.zone.desc = *desc;
  if (c.zone.desc.decay_s <= 0) c.zone.desc.decay_s = 1.5f;
  send(&c);
}

void cw_audio_remove_reverb_zone(cw_reverb_zone zone) {
  if (!zone_ok(zone)) return;
  cw__cmd c = { .type = CW__CMD_ZONE_REMOVE, .slot = (int16_t)zone };
  send(&c);
  cw__au.zone_used[zone] = false;
}

float cw_audio_reverb_zone_weight(cw_reverb_zone zone) {
  if (!zone_ok(zone)) return 0;
  uint32_t b = atomic_load_explicit(&cw__au.mix.pub_zone_w[zone], memory_order_relaxed);
  float w;
  memcpy(&w, &b, 4);
  return w;
}

/* ── buses ──────────────────────────────────────────────────────────── */

static bool bus_ok(cw_bus b) { return cw__au.ready && b >= 0 && b < CW_AUDIO_MAX_BUSES && cw__au.buses[b].used; }

/* Solo: a bus stays audible when nothing is soloed, or when it, an ancestor
 * or a descendant is soloed. Mute always wins. */
static void update_audibility(void) {
  cw__gbus *bs = cw__au.buses;
  bool any_solo = false, keep[CW_AUDIO_MAX_BUSES] = { 0 };
  for (int b = 0; b < CW_AUDIO_MAX_BUSES; b++) {
    if (!bs[b].used || !bs[b].solo) continue;
    any_solo = true;
    for (int a = b;; a = bs[a].parent) { /* it and its ancestors */
      keep[a] = true;
      if (a == 0) break;
    }
  }
  for (int b = 1; b < CW_AUDIO_MAX_BUSES; b++) { /* descendants of a soloed bus */
    if (!bs[b].used) continue;
    for (int a = bs[b].parent;; a = bs[a].parent) {
      if (bs[a].solo) { keep[b] = true; break; }
      if (a == 0) break;
    }
  }
  for (int b = 0; b < CW_AUDIO_MAX_BUSES; b++) {
    if (!bs[b].used) continue;
    cw__cmd c = { .type = CW__CMD_BUS_AUDIBLE, .slot = (int16_t)b };
    c.f.flag = !bs[b].mute && (!any_solo || keep[b]);
    send(&c);
  }
}

cw_bus cw_bus_create(const char *name, cw_bus parent) {
  if (!bus_ok(parent)) {
    cw_error("audio: cw_bus_create('%s'): parent bus %d does not exist", name ? name : "", parent);
    return -1;
  }
  for (int b = 1; b < CW_AUDIO_MAX_BUSES; b++) {
    if (cw__au.buses[b].used) continue;
    cw__au.buses[b] = (cw__gbus){ .name = cw_intern(name ? name : ""), .used = true, .parent = parent };
    cw__cmd c = { .type = CW__CMD_BUS_SETUP, .slot = (int16_t)b };
    c.bus.parent = parent;
    send(&c);
    update_audibility();
    return b;
  }
  cw_error("audio: all %d buses in use", CW_AUDIO_MAX_BUSES);
  return -1;
}

cw_bus cw_bus_find(const char *name) {
  if (!cw__au.ready || !name) return -1;
  const char *n = cw_intern(name);
  for (int b = 0; b < CW_AUDIO_MAX_BUSES; b++)
    if (cw__au.buses[b].used && cw__au.buses[b].name == n) return b;
  return -1;
}

void cw_bus_set_volume(cw_bus b, float volume) {
  if (!bus_ok(b)) return;
  cw__cmd c = { .type = CW__CMD_BUS_VOLUME, .slot = (int16_t)b };
  c.f.a = volume < 0 ? 0 : volume;
  send(&c);
}
void cw_bus_set_mute(cw_bus b, bool mute) {
  if (!bus_ok(b)) return;
  cw__au.buses[b].mute = mute;
  update_audibility();
}
void cw_bus_set_solo(cw_bus b, bool solo) {
  if (!bus_ok(b)) return;
  cw__au.buses[b].solo = solo;
  update_audibility();
}

void cw_bus_set_send(cw_bus b, int slot, cw_bus target, float level) {
  if (!bus_ok(b) || slot < 0 || slot >= CW_AUDIO_MAX_SENDS) return;
  if (level > 0 && (!bus_ok(target) || target >= b)) {
    cw_error("audio: bus %d can only send to a bus created before it (got %d)", b, target);
    return;
  }
  cw__cmd c = { .type = CW__CMD_BUS_SEND, .slot = (int16_t)b };
  c.send.slot = slot;
  c.send.target = level > 0 ? target : -1;
  c.send.level = level > 0 ? level : 0;
  send(&c);
}

int cw_bus_add_fx(cw_bus b, const cw_fx_desc *fx) {
  if (!bus_ok(b) || !fx) return -1;
  if (cw__au.buses[b].nfx >= CW_AUDIO_MAX_FX) {
    cw_error("audio: bus %d already has %d effects", b, CW_AUDIO_MAX_FX);
    return -1;
  }
  cw__fx *f = cw__fx_create(fx, cw__au.mix.rate);
  if (!f) return -1;
  cw__cmd c = { .type = CW__CMD_FX_ADD, .slot = (int16_t)b };
  c.fx.fx = f;
  send(&c);
  return cw__au.buses[b].nfx++;
}

void cw_bus_set_fx(cw_bus b, int slot, const cw_fx_desc *fx) {
  if (!bus_ok(b) || slot < 0 || slot >= cw__au.buses[b].nfx || !fx) return;
  cw__cmd c = { .type = CW__CMD_FX_SET, .slot = (int16_t)b };
  c.fx.slot = slot;
  c.fx.desc = *fx;
  send(&c);
}

void cw_bus_set_fx_bypass(cw_bus b, int slot, bool bypass) {
  if (!bus_ok(b) || slot < 0 || slot >= cw__au.buses[b].nfx) return;
  cw__cmd c = { .type = CW__CMD_FX_BYPASS, .slot = (int16_t)b };
  c.fx.slot = slot;
  c.fx.bypass = bypass;
  send(&c);
}

void cw_bus_remove_fx(cw_bus b, int slot) {
  if (!bus_ok(b) || slot < 0 || slot >= cw__au.buses[b].nfx) return;
  cw__cmd c = { .type = CW__CMD_FX_REMOVE, .slot = (int16_t)b };
  c.fx.slot = slot;
  send(&c);
  cw__au.buses[b].nfx--;
}

/* ── system ─────────────────────────────────────────────────────────── */

static void fill_cb(void *user, float *out, int frames) {
  (void)user;
  cw__mixer_mix(out, frames);
}
/* the platform's fence (cw__audio_fence_hook): commands queued so far are
 * the mix's, later ones wait */
extern void (*cw__audio_fence_hook)(bool on);
static void fence_commands(bool on) {
  cw__au.cmd_fence = atomic_load_explicit(&cw__au.cmds.tail, memory_order_relaxed);
  cw__au.fenced = on;
}
static void silence_cb(void *user, float *out, int frames) {
  (void)user;
  memset(out, 0, (size_t)frames * 2 * sizeof(float));
}

bool cw_audio_init(const cw_audio_desc *desc) {
  if (cw__au.ready) {
    cw_error("audio: cw_audio_init called twice");
    return false;
  }
  cw__audio *a = &cw__au;
  memset(a, 0, sizeof *a);
  cw_audio_desc d = desc ? *desc : (cw_audio_desc){ 0 };
  if (d.sample_rate <= 0) d.sample_rate = 48000;
  if (d.max_voices <= 0) d.max_voices = 64;
  if (d.max_voices > 4096) d.max_voices = 4096;
  if (d.max_streams <= 0) d.max_streams = 8;
  if (d.stream_ring_frames <= 0) d.stream_ring_frames = 16384;
  if (d.stream_ring_frames < 4096) d.stream_ring_frames = 4096; /* a block at pitch 8 needs 2049 */
  if (d.command_capacity <= 0) d.command_capacity = 4096;
  a->desc = d;
  a->offline = d.offline;
  int rate = d.sample_rate;
  a->stats.device = false;
  if (!d.offline) {
    /* the device decides the rate: open it silent first; the mixer takes
     * over the callback once it exists */
    a->stats.device = cw_audio_open(rate, silence_cb, NULL);
    if (cw_audio_rate() > 0) rate = cw_audio_rate();
  }
  cw__mixer_init(&a->mix, rate, d.max_voices);
  cw__spsc_init(&a->cmds, sizeof(cw__cmd), (uint32_t)d.command_capacity);
  cw__spsc_init(&a->events, sizeof(cw__event), (uint32_t)(d.max_voices * 2 + 64));
  cw_pool_init(&a->sounds, sizeof(cw__sound), CW_MEM_AUDIO);
  a->gvoices = cw_calloc((size_t)d.max_voices, sizeof *a->gvoices, CW_MEM_AUDIO);
  a->max_streams = d.max_streams;
  a->streams = cw_calloc((size_t)d.max_streams, sizeof *a->streams, CW_MEM_AUDIO);
  uint32_t cap = 1;
  while (cap < (uint32_t)d.stream_ring_frames) cap <<= 1;
  for (int i = 0; i < d.max_streams; i++) {
    a->streams[i].cap = cap;
    a->streams[i].ring = cw_calloc((size_t)cap * 2, sizeof(float), CW_MEM_AUDIO);
    a->streams[i].voice_slot = -1;
    atomic_store(&a->streams[i].end, UINT64_MAX);
  }
  a->buses[0] = (cw__gbus){ .name = cw_intern("master"), .used = true };
  a->hrtf_limit = a->mix.hrtf_limit = CW_MIN(16, d.max_voices);
  a->ready = true;
  cw__audio_fence_hook = fence_commands;
  cw_debug_u32("audio.mixed_frames", &a->dbg_mixed);
  cw_debug_u32("audio.underruns", &a->dbg_underruns);
  cw_debug_i32("audio.voices", &a->dbg_voices);
  cw_debug_u32("audio.dropped_commands", &a->dbg_dropped);
  if (!d.offline && a->stats.device) {
    if (!cw_audio_open(rate, fill_cb, NULL)) a->stats.device = false;
  }
  cw_info("audio: %d Hz, %d voices, %d streams%s", rate, d.max_voices, d.max_streams,
          d.offline ? " (offline)" : a->stats.device ? "" : " (no device: silent)");
  return true;
}

void cw_audio_shutdown(void) {
  cw__audio *a = &cw__au;
  if (!a->ready) return;
  if (!a->offline) cw_audio_close(); /* a cart's mix job is done after this */
  cw__audio_fence_hook = NULL;
  for (int i = 0; i < a->max_streams; i++) {
    if (a->streams[i].in_use) stream_release(i);
    cw_free(a->streams[i].ring);
  }
  cw_free(a->streams);
  /* effects still attached to buses */
  for (int b = 0; b < CW_AUDIO_MAX_BUSES; b++)
    for (int f = 0; f < a->mix.buses[b].nfx; f++) cw__fx_destroy(a->mix.buses[b].fx[f]);
  cw__event e;
  while (cw__spsc_pop(&a->events, &e)) {
    if (e.type == CW__EV_FX_FREED) cw__fx_destroy(e.fx);
    if (e.type == CW__EV_HRTF_FREED) cw__hrtf_free(e.hrtf);
  }
  cw__cmd c; /* commands never applied may own an effect or a table */
  while (cw__spsc_pop(&a->cmds, &c)) {
    if (c.type == CW__CMD_FX_ADD) cw__fx_destroy(c.fx.fx);
    if (c.type == CW__CMD_HRTF) cw__hrtf_free(c.hrtf);
    if (c.type == CW__CMD_ZONE_ADD) cw__fx_destroy(c.zone.fx);
  }
  for (int z = 0; z < CW_AUDIO_MAX_REVERB_ZONES; z++)
    if (a->mix.zones[z].used) cw__fx_destroy(a->mix.zones[z].fx);
  cw__hrtf_free(a->mix.hrtf);
  a->mix.hrtf = NULL;
  for (int i = 0; i < a->sounds.slot_count; i++) {
    cw__sound *s = cw_pool_at(&a->sounds, i);
    if (s) {
      cw_free(s->pcm);
      cw_free(s->data);
    }
  }
  cw_pool_free_all(&a->sounds);
  cw_free(a->gvoices);
  cw__spsc_free(&a->cmds);
  cw__spsc_free(&a->events);
  cw__mixer_free(&a->mix);
  a->ready = false;
}

int cw_audio_sample_rate(void) { return cw__au.ready ? cw__au.mix.rate : 0; }

void cw_audio_update(void) {
  cw__audio *a = &cw__au;
  if (!a->ready) return;
  cw__event e;
  while (cw__spsc_pop(&a->events, &e)) {
    if (e.type == CW__EV_FX_FREED) {
      cw__fx_destroy(e.fx);
      continue;
    }
    if (e.type == CW__EV_HRTF_FREED) {
      cw__hrtf_free(e.hrtf);
      continue;
    }
    cw__gvoice *g = &a->gvoices[e.slot];
    cw__sound *snd = sound_by_id(e.sound_id);
    if (g->live && g->gen == e.gen) { /* ended on its own (or stopped), not stolen */
      g->live = false;
      if (snd) snd->live--;
    }
    if (e.stream >= 0) stream_release(e.stream);
    if (snd && --snd->refs == 0 && snd->released) sound_destroy(e.sound_id);
  }
  if (!a->offline) pump_streams();
  cw__mixer *m = &a->mix;
  a->stats.mixed_frames = atomic_load_explicit(&m->pub_mixed, memory_order_acquire);
  a->stats.blocks = atomic_load_explicit(&m->pub_blocks, memory_order_relaxed);
  a->stats.underruns = atomic_load_explicit(&m->pub_underruns, memory_order_relaxed);
  a->stats.underrun_frames = atomic_load_explicit(&m->pub_underrun_frames, memory_order_relaxed);
  a->stats.voices = atomic_load_explicit(&m->pub_voices, memory_order_relaxed);
  a->stats.streams = atomic_load_explicit(&m->pub_streams, memory_order_relaxed);
  a->stats.hrtf_voices = atomic_load_explicit(&m->pub_hrtf, memory_order_relaxed);
  uint32_t pb = atomic_load_explicit(&m->pub_peak_bits, memory_order_relaxed);
  memcpy(&a->stats.peak, &pb, 4);
  a->stats.dropped_commands = a->dropped;
  a->dbg_mixed = (uint32_t)a->stats.mixed_frames;
  a->dbg_underruns = a->stats.underruns;
  a->dbg_voices = a->stats.voices;
  a->dbg_dropped = a->dropped;
}

void cw_audio_render(float *out, int frames) {
  if (!cw__au.ready || !cw__au.offline) {
    CW_ASSERT(false, "cw_audio_render needs cw_audio_init({.offline = true})");
    return;
  }
  cw__mixer_mix(out, frames);
  cw_audio_update();
}

void cw_audio_get_stats(cw_audio_stats *out) { *out = cw__au.stats; }

/* ── test helpers ───────────────────────────────────────────────────── */

cw_audio_analysis cw_audio_analyze(const float *samples, size_t count) {
  cw_audio_analysis r = { .hash = cw_hash32(samples, count * sizeof(float), CW_HASH32_INIT) };
  double sum = 0;
  for (size_t i = 0; i < count; i++) {
    float x = samples[i], ax = x < 0 ? -x : x;
    if (ax > r.peak) r.peak = ax;
    sum += (double)x * (double)x;
  }
  r.rms = count ? (float)sqrt(sum / (double)count) : 0.0f;
  return r;
}

int64_t cw_audio_decode_asset(const char *path, float **pcm, int *channels, int *rate) {
  size_t len = 0;
  void *data = cw_asset_load(path, &len);
  *pcm = NULL;
  if (!data) return -1;
  cw__decoder dec;
  if (!cw__dec_open(&dec, data, len)) {
    free(data);
    return -1;
  }
  int64_t n = decode_all(&dec, pcm);
  *channels = dec.channels;
  *rate = dec.rate;
  cw__dec_close(&dec);
  free(data);
  return n;
}

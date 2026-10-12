/* The mixer: everything that runs inside the audio fill callback.
 *
 * Real-time rules, all of them load-bearing: no locks, no allocation, no
 * frees, no waiting. Input arrives as commands on an SPSC queue; voice
 * endings and memory the mixer lets go of leave as events on another. Before
 * applying a command the mixer makes sure the event queue has room for what
 * that command could post; when it has not (the game stopped calling
 * cw_audio_update), commands simply wait in their queue.
 *
 * Determinism: positions are 32.32 fixed point, gains ramp linearly across a
 * block, and every transcendental is a cartwheel/math.h function, so the
 * same commands at the same block boundaries give bit-identical output on
 * every host. */
#include <string.h>

#include "internal.h"

static inline cw__mixer *M(void) { return &cw__au.mix; }

void cw__mixer_init(cw__mixer *m, int rate, int max_voices) {
  memset(m, 0, sizeof *m);
  m->rate = rate;
  m->max_voices = max_voices;
  m->voices = cw_calloc((size_t)max_voices, sizeof *m->voices, CW_MEM_AUDIO);
  m->clock_gen = cw_calloc((size_t)max_voices, sizeof *m->clock_gen, CW_MEM_AUDIO);
  m->clock = cw_calloc((size_t)max_voices, sizeof *m->clock, CW_MEM_AUDIO);
  m->scratch = cw_calloc(CW_AUDIO_BLOCK * 2, sizeof(float), CW_MEM_AUDIO);
  for (int b = 0; b < CW_AUDIO_MAX_BUSES; b++) {
    m->buses[b].buf = cw_calloc(CW_AUDIO_BLOCK * 2, sizeof(float), CW_MEM_AUDIO);
    m->buses[b].volume = m->buses[b].cur_volume = 1.0f;
    m->buses[b].audible = true;
    for (int s = 0; s < CW_AUDIO_MAX_SENDS; s++) m->buses[b].sends[s].target = -1;
  }
  m->buses[0].used = true;
  for (int l = 0; l < CW_AUDIO_MAX_LISTENERS; l++) {
    m->lfwd[l] = (t3_vec3){ 0, 0, -1 };
    m->lup[l] = (t3_vec3){ 0, 1, 0 };
  }
  m->nlisteners = 1;
  m->speed_of_sound = 343.0f;
}

void cw__mixer_free(cw__mixer *m) {
  cw_free(m->voices);
  cw_free((void *)m->clock_gen);
  cw_free((void *)m->clock);
  cw_free(m->scratch);
  for (int b = 0; b < CW_AUDIO_MAX_BUSES; b++) cw_free(m->buses[b].buf);
  memset(m, 0, sizeof *m);
}

/* ── small vector helpers (no libm) ─────────────────────────────────── */

static inline t3_vec3 vsub(t3_vec3 a, t3_vec3 b) { return (t3_vec3){ a.x - b.x, a.y - b.y, a.z - b.z }; }
static inline float vdot(t3_vec3 a, t3_vec3 b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
static inline t3_vec3 vcross(t3_vec3 a, t3_vec3 b) {
  return (t3_vec3){ a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x };
}
static inline float vlen(t3_vec3 a) { return sqrtf(vdot(a, a)); }
static inline t3_vec3 vscale(t3_vec3 a, float s) { return (t3_vec3){ a.x * s, a.y * s, a.z * s }; }

/* ── events ─────────────────────────────────────────────────────────── */

static void end_voice(int slot) {
  cw__mvoice *v = &M()->voices[slot];
  cw__event e = { .type = CW__EV_VOICE_ENDED, .slot = (int16_t)slot, .stream = (int16_t)v->stream, .gen = v->gen,
                  .sound_id = v->sound_id };
  v->state = cw__spsc_push(&cw__au.events, &e) ? CW__V_FREE : CW__V_ZOMBIE;
}

/* ── commands ───────────────────────────────────────────────────────── */

static cw__mvoice *live_voice(const cw__cmd *c) {
  if (c->slot < 0 || c->slot >= M()->max_voices) return NULL;
  cw__mvoice *v = &M()->voices[c->slot];
  return (v->state == CW__V_WAITING || v->state == CW__V_PLAYING) && v->gen == c->gen ? v : NULL;
}

static void start_voice(const cw__cmd *c) {
  cw__mixer *m = M();
  cw__mvoice *v = &m->voices[c->slot];
  if (v->state != CW__V_FREE) end_voice(c->slot); /* stolen (or a zombie): report the old one */
  const cw__play *p = &c->play;
  memset(v, 0, sizeof *v);
  v->state = CW__V_WAITING;
  v->gen = c->gen;
  v->sound_id = p->sound_id;
  v->pcm = p->pcm;
  v->frames = p->frames;
  v->channels = p->channels;
  v->stream = p->stream;
  v->ratio = p->ratio;
  v->volume = p->d.volume;
  v->pitch = p->d.pitch;
  v->pan = p->d.pan;
  v->loop = p->d.loop;
  v->loop_start = p->d.loop_start;
  v->loop_end = p->d.loop_end;
  v->paused = p->d.paused;
  v->bus = p->d.bus;
  v->spatial = p->d.spatial;
  v->sp = p->d.space;
  v->start_frame = p->d.start_frame;
  v->priming = p->stream >= 0;
  if (p->d.fade_in > 0) {
    v->fade = 0;
    v->fade_target = 1;
    v->fade_step = 1.0f / (p->d.fade_in * (float)m->rate);
  } else {
    v->fade = v->fade_target = 1;
  }
  if (p->stream >= 0) {
    v->ipos = atomic_load_explicit(&cw__au.streams[p->stream].read, memory_order_acquire);
    v->played = (uint64_t)p->d.offset;
  } else {
    v->ipos = (uint64_t)p->d.offset;
    v->played = (uint64_t)p->d.offset;
  }
  atomic_store_explicit(&m->clock[c->slot], v->played, memory_order_release);
  atomic_store_explicit(&m->clock_gen[c->slot], v->gen, memory_order_release);
}

static void apply(const cw__cmd *c) {
  cw__mixer *m = M();
  cw__mvoice *v;
  switch ((cw__cmd_type)c->type) {
  case CW__CMD_PLAY: start_voice(c); break;
  case CW__CMD_STOP:
    if (!(v = live_voice(c))) break;
    if (c->f.a <= 0 || v->state == CW__V_WAITING || v->paused) end_voice(c->slot);
    else {
      v->fade_target = 0;
      v->fade_step = 1.0f / (c->f.a * (float)m->rate);
      v->stop_at_fade_end = true;
    }
    break;
  case CW__CMD_PAUSE: if ((v = live_voice(c))) v->paused = c->f.flag; break;
  case CW__CMD_VOLUME: if ((v = live_voice(c))) v->volume = c->f.a; break;
  case CW__CMD_PITCH: if ((v = live_voice(c))) v->pitch = c->f.a; break;
  case CW__CMD_PAN: if ((v = live_voice(c))) v->pan = c->f.a; break;
  case CW__CMD_BUS: if ((v = live_voice(c))) v->bus = (int)c->f.a; break;
  case CW__CMD_FADE:
    if (!(v = live_voice(c))) break;
    v->fade_target = cw_clampf(c->f.a, 0, 1);
    if (c->f.b <= 0) {
      v->fade = v->fade_target;
      v->fade_step = 0;
    } else {
      float d = v->fade_target - v->fade;
      v->fade_step = (d < 0 ? -d : d) / (c->f.b * (float)m->rate);
    }
    break;
  case CW__CMD_3D:
    if (!(v = live_voice(c))) break;
    v->sp = c->space;
    v->spatial = true;
    break;
  case CW__CMD_POSITION:
    if (!(v = live_voice(c))) break;
    v->sp.position = c->pos.p;
    v->sp.velocity = c->pos.v;
    break;
  case CW__CMD_OCCLUSION: if ((v = live_voice(c))) v->occlusion = cw_clampf(c->f.a, 0, 1); break;
  case CW__CMD_LISTENER: {
    int l = c->listener.index;
    if (c->listener.count > 0) m->nlisteners = c->listener.count;
    if (l < 0 || l >= CW_AUDIO_MAX_LISTENERS) break;
    m->lpos[l] = c->listener.p;
    m->lvel[l] = c->listener.v;
    m->lfwd[l] = c->listener.fwd;
    m->lup[l] = c->listener.up;
    break;
  }
  case CW__CMD_SPEED_OF_SOUND: m->speed_of_sound = c->f.a > 0 ? c->f.a : 343.0f; break;
  case CW__CMD_BUS_SETUP: {
    cw__mbus *b = &m->buses[c->slot];
    b->used = true;
    b->parent = c->bus.parent;
    b->volume = b->cur_volume = 1;
    break;
  }
  case CW__CMD_BUS_VOLUME: m->buses[c->slot].volume = c->f.a; break;
  case CW__CMD_BUS_AUDIBLE: m->buses[c->slot].audible = c->f.flag; break;
  case CW__CMD_BUS_SEND: {
    cw__mbus *b = &m->buses[c->slot];
    if (b->sends[c->send.slot].target != c->send.target) b->sends[c->send.slot].cur = 0; /* ramp in */
    b->sends[c->send.slot].target = c->send.target;
    b->sends[c->send.slot].level = c->send.level;
    break;
  }
  case CW__CMD_FX_ADD: {
    cw__mbus *b = &m->buses[c->slot];
    b->fx[b->nfx++] = c->fx.fx;
    break;
  }
  case CW__CMD_FX_SET: cw__fx_set(m->buses[c->slot].fx[c->fx.slot], &c->fx.desc); break;
  case CW__CMD_FX_BYPASS: m->buses[c->slot].fx[c->fx.slot]->bypass = c->fx.bypass; break;
  case CW__CMD_FX_REMOVE: {
    cw__mbus *b = &m->buses[c->slot];
    cw__event e = { .type = CW__EV_FX_FREED, .fx = b->fx[c->fx.slot] };
    cw__spsc_push(&cw__au.events, &e); /* room was checked before apply() */
    for (int i = c->fx.slot; i < b->nfx - 1; i++) b->fx[i] = b->fx[i + 1];
    b->fx[--b->nfx] = NULL;
    break;
  }
  }
}

static void drain_commands(void) {
  cw__cmd c;
  /* a PLAY can end the voice it replaces; FX_REMOVE frees one: 2 is enough */
  while (cw__spsc_free_slots(&cw__au.events) >= 2) {
    if (cw__au.fenced && atomic_load_explicit(&cw__au.cmds.head, memory_order_relaxed) == cw__au.cmd_fence) break;
    if (!cw__spsc_pop(&cw__au.cmds, &c)) break;
    apply(&c);
  }
}

/* ── one voice ──────────────────────────────────────────────────────── */

/* Per-block gains and pitch (including the 3D model). */
static void voice_params(cw__mixer *m, cw__mvoice *v, float fade_end, float *gl, float *gr, float *ratio) {
  float g = v->volume * fade_end;
  float pan = v->pan;
  float dop = 1.0f;
  if (v->spatial) {
    const cw_voice_3d *s = &v->sp;
    /* several listeners (splitscreen): each voice is heard by the nearest */
    int li = 0;
    t3_vec3 d = vsub(s->position, m->lpos[0]);
    float dist = vlen(d);
    for (int l = 1; l < m->nlisteners; l++) {
      t3_vec3 dl = vsub(s->position, m->lpos[l]);
      float ll = vlen(dl);
      if (ll < dist) d = dl, dist = ll, li = l;
    }
    float ref = s->ref_distance > 0 ? s->ref_distance : 1.0f;
    float maxd = s->max_distance > 0 ? s->max_distance : 1000.0f;
    float roll = s->rolloff > 0 ? s->rolloff : 1.0f;
    float dc = cw_clampf(dist, ref, CW_MAX(ref, maxd));
    float att = 1.0f;
    switch (s->model) {
    case CW_ATTEN_INVERSE: att = ref / (ref + roll * (dc - ref)); break;
    case CW_ATTEN_LINEAR: att = maxd > ref ? 1.0f - roll * (dc - ref) / (maxd - ref) : 1.0f; break;
    case CW_ATTEN_EXPONENTIAL: att = cw_pow(dc / ref, -roll); break;
    default: break;
    }
    g *= cw_clampf(att, 0, 1);
    t3_vec3 dn = dist > 1e-6f ? vscale(d, 1.0f / dist) : (t3_vec3){ 0, 0, 0 };
    /* cone: angle between the source's axis and the direction to the listener */
    float inner = s->cone_inner_deg > 0 ? s->cone_inner_deg : 360.0f;
    float outer = s->cone_outer_deg > 0 ? s->cone_outer_deg : 360.0f;
    float axis_len = vlen(s->direction);
    if (inner < 360.0f && axis_len > 1e-6f && dist > 1e-6f) {
      float cs = cw_clampf(-vdot(vscale(s->direction, 1.0f / axis_len), dn), -1, 1);
      float angle = 2.0f * cw_acos(cs) * (180.0f / CW_PI); /* full angle the listener sits in */
      float cone = 1.0f;
      if (angle >= outer) cone = s->cone_outer_gain;
      else if (angle > inner) cone = 1.0f + (s->cone_outer_gain - 1.0f) * (angle - inner) / (outer - inner);
      g *= cone;
    }
    /* pan from the listener-relative direction */
    t3_vec3 right = vcross(m->lfwd[li], m->lup[li]);
    float rl = vlen(right);
    pan = rl > 1e-6f ? cw_clampf(vdot(dn, right) / rl, -1, 1) : 0.0f;
    /* doppler (OpenAL's formula): SL points from source to listener */
    if (s->doppler > 0 && dist > 1e-6f) {
      t3_vec3 sl = vscale(dn, -1.0f);
      float c = m->speed_of_sound, lim = c / s->doppler;
      float vls = cw_clampf(vdot(sl, m->lvel[li]), -lim, lim);
      float vss = cw_clampf(vdot(sl, s->velocity), -lim, lim);
      float den = c - s->doppler * vss;
      if (den > 1e-3f) dop = (c - s->doppler * vls) / den;
    }
  }
  if (v->spatial || v->channels == 1) { /* equal power */
    float sn, cs;
    cw_sincos((pan + 1.0f) * (CW_PI * 0.25f), &sn, &cs);
    *gl = g * cs;
    *gr = g * sn;
  } else { /* stereo balance */
    *gl = g * (pan > 0 ? 1.0f - pan : 1.0f);
    *gr = g * (pan < 0 ? 1.0f + pan : 1.0f);
  }
  *ratio = cw_clampf(v->ratio * v->pitch * dop, 1.0f / 256.0f, 8.0f);
}

/* Renders up to n frames of source into out (stereo, ramped gains). Returns
 * true when the source ended inside. */
static bool render_voice(cw__mixer *m, cw__mvoice *v, float *out, int n, float gl0, float gr0, float gl1, float gr1,
                         float ratio) {
  uint64_t step = (uint64_t)((double)ratio * 4294967296.0);
  uint32_t step_lo = (uint32_t)step;
  uint64_t step_hi = step >> 32;
  int ch = v->channels;
  bool mono_mix = v->spatial && ch == 2;
  float dgl = (gl1 - gl0) / (float)n, dgr = (gr1 - gr0) / (float)n;
  float gl = gl0, gr = gr0;
  bool lp = v->occlusion > 0;
  float lpa = 1.0f;
  if (lp) {
    float fc = 20000.0f * cw_pow(0.015f, v->occlusion); /* 20 kHz .. 300 Hz */
    lpa = 1.0f - cw_exp(-CW_TAU * fc / (float)m->rate);
  }
  cw__stream *st = v->stream >= 0 ? &cw__au.streams[v->stream] : NULL;
  uint64_t write = 0, end = UINT64_MAX;
  uint32_t mask = 0;
  if (st) {
    write = atomic_load_explicit(&st->write, memory_order_acquire);
    end = atomic_load_explicit(&st->end, memory_order_acquire);
    mask = st->cap - 1;
  }
  bool ended = false;
  int i = 0;
  for (; i < n; i++) {
    float a0, a1, b0, b1;
    uint64_t i0 = v->ipos, i1 = i0 + 1;
    if (st) {
      if (i0 >= end) { ended = true; break; }
      if (i1 >= write && i1 < end) { /* the ring ran dry */
        atomic_fetch_add_explicit(&m->pub_underruns, 1, memory_order_relaxed);
        atomic_fetch_add_explicit(&m->pub_underrun_frames, (uint32_t)(n - i), memory_order_relaxed);
        break;
      }
      const float *pa = st->ring + (i0 & mask) * (uint32_t)ch;
      a0 = pa[0];
      a1 = ch == 2 ? pa[1] : a0;
      if (i1 < end) {
        const float *pb = st->ring + (i1 & mask) * (uint32_t)ch;
        b0 = pb[0];
        b1 = ch == 2 ? pb[1] : b0;
      } else b0 = b1 = 0;
    } else {
      int64_t le = v->loop_end > 0 && v->loop_end <= v->frames ? v->loop_end : v->frames;
      if ((int64_t)i0 >= le) { ended = true; break; }
      if (v->loop && (int64_t)i1 >= le) i1 = (uint64_t)v->loop_start;
      const float *pa = v->pcm + i0 * (uint64_t)ch;
      a0 = pa[0];
      a1 = ch == 2 ? pa[1] : a0;
      if ((int64_t)i1 < v->frames) {
        const float *pb = v->pcm + i1 * (uint64_t)ch;
        b0 = pb[0];
        b1 = ch == 2 ? pb[1] : b0;
      } else b0 = b1 = 0;
    }
    float t = (float)(v->frac >> 8) * (1.0f / 16777216.0f);
    float s0 = a0 + (b0 - a0) * t, s1 = a1 + (b1 - a1) * t;
    if (mono_mix) s0 = s1 = (s0 + s1) * 0.5f;
    if (lp) {
      v->lp[0] += lpa * (s0 - v->lp[0]);
      v->lp[1] += lpa * (s1 - v->lp[1]);
      s0 = v->lp[0];
      s1 = v->lp[1];
    } else {
      v->lp[0] = s0;
      v->lp[1] = s1;
    }
    gl += dgl;
    gr += dgr;
    out[2 * i] += s0 * gl;
    out[2 * i + 1] += s1 * gr;
    /* advance */
    uint32_t f = v->frac + step_lo;
    uint64_t adv = step_hi + (f < v->frac ? 1 : 0);
    v->frac = f;
    v->ipos += adv;
    v->played += adv;
    if (!st && v->loop) {
      int64_t le = v->loop_end > 0 && v->loop_end <= v->frames ? v->loop_end : v->frames;
      int64_t len = le - v->loop_start;
      if (len > 0)
        while ((int64_t)v->ipos >= le) v->ipos -= (uint64_t)len;
    }
  }
  if (st) atomic_store_explicit(&st->read, v->ipos < write ? v->ipos : write, memory_order_release);
  return ended;
}

/* ── one block ──────────────────────────────────────────────────────── */

static void mix_block(float *out, int n) {
  cw__mixer *m = M();
  uint64_t F = m->mixed;
  drain_commands();
  for (int b = 0; b < CW_AUDIO_MAX_BUSES; b++)
    if (m->buses[b].used) memset(m->buses[b].buf, 0, (size_t)n * 2 * sizeof(float));
  int nvoices = 0, nstreams = 0;
  for (int s = 0; s < m->max_voices; s++) {
    cw__mvoice *v = &m->voices[s];
    if (v->state == CW__V_ZOMBIE) { end_voice(s); continue; }
    if (v->state == CW__V_FREE) continue;
    int off = 0;
    if (v->state == CW__V_WAITING) {
      if (v->start_frame >= (int64_t)(F + (uint64_t)n)) continue;
      if (v->start_frame > (int64_t)F) off = (int)(v->start_frame - (int64_t)F);
      v->state = CW__V_PLAYING;
    }
    if (v->paused) continue;
    if (v->priming) {
      cw__stream *st = &cw__au.streams[v->stream];
      uint64_t have = atomic_load_explicit(&st->write, memory_order_acquire) - v->ipos;
      bool ended = atomic_load_explicit(&st->end, memory_order_acquire) != UINT64_MAX;
      if (!ended && have < CW_MIN((uint64_t)st->cap / 2, (uint64_t)2048)) continue; /* ~43 ms buffered */
      v->priming = false;
    }
    int cnt = n - off;
    /* fade across this block */
    float fade_end = v->fade;
    if (v->fade_step > 0) {
      float d = v->fade_step * (float)cnt;
      fade_end = v->fade < v->fade_target ? CW_MIN(v->fade + d, v->fade_target) : CW_MAX(v->fade - d, v->fade_target);
    }
    float gl1, gr1, ratio;
    voice_params(m, v, fade_end, &gl1, &gr1, &ratio);
    float gl0 = gl1, gr0 = gr1;
    if (v->gains_valid) {
      gl0 = v->gl;
      gr0 = v->gr;
    } else if (v->fade_step > 0 || fade_end != v->fade) { /* first block of a fade: ramp from the start level */
      voice_params(m, v, v->fade, &gl0, &gr0, &ratio);
      voice_params(m, v, fade_end, &gl1, &gr1, &ratio);
    }
    int bus = v->bus >= 0 && v->bus < CW_AUDIO_MAX_BUSES && m->buses[v->bus].used ? v->bus : 0;
    bool ended = render_voice(m, v, m->buses[bus].buf + off * 2, cnt, gl0, gr0, gl1, gr1, ratio);
    v->gl = gl1;
    v->gr = gr1;
    v->gains_valid = true;
    v->fade = fade_end;
    if (v->fade == v->fade_target) v->fade_step = 0;
    atomic_store_explicit(&m->clock[s], v->played, memory_order_release);
    if (ended || (v->stop_at_fade_end && v->fade <= 0)) end_voice(s);
  }
  for (int s = 0; s < m->max_voices; s++) {
    const cw__mvoice *v = &m->voices[s];
    if (v->state != CW__V_WAITING && v->state != CW__V_PLAYING) continue;
    nvoices++;
    nstreams += v->stream >= 0;
  }
  /* buses: children were created after their parents, so reverse index
   * order processes every bus after everything that feeds it */
  for (int b = CW_AUDIO_MAX_BUSES - 1; b >= 0; b--) {
    cw__mbus *bus = &m->buses[b];
    if (!bus->used) continue;
    for (int f = 0; f < bus->nfx; f++) cw__fx_process(bus->fx[f], bus->buf, n);
    float target = bus->audible ? bus->volume : 0.0f;
    float g = bus->cur_volume, dg = (target - g) / (float)n;
    for (int i = 0; i < n; i++) {
      g += dg;
      bus->buf[2 * i] *= g;
      bus->buf[2 * i + 1] *= g;
    }
    bus->cur_volume = target;
    if (b == 0) break;
    float *dst = m->buses[bus->parent].buf;
    for (int i = 0; i < n * 2; i++) dst[i] += bus->buf[i];
    for (int s = 0; s < CW_AUDIO_MAX_SENDS; s++) {
      int t = bus->sends[s].target;
      if (t < 0 || t >= b || !m->buses[t].used) continue;
      float sg = bus->sends[s].cur, ds = (bus->sends[s].level - sg) / (float)n;
      float *sd = m->buses[t].buf;
      for (int i = 0; i < n; i++) {
        sg += ds;
        sd[2 * i] += bus->buf[2 * i] * sg;
        sd[2 * i + 1] += bus->buf[2 * i + 1] * sg;
      }
      bus->sends[s].cur = bus->sends[s].level;
    }
  }
  float peak = 0;
  const float *mb = m->buses[0].buf;
  for (int i = 0; i < n * 2; i++) {
    float x = mb[i];
    if (!(x == x)) x = 0; /* NaN never reaches the device */
    float ax = x < 0 ? -x : x;
    if (ax > peak) peak = ax;
    out[i] = cw_clampf(x, -1.0f, 1.0f);
  }
  m->mixed += (uint64_t)n;
  uint32_t pb;
  memcpy(&pb, &peak, 4);
  atomic_store_explicit(&m->pub_peak_bits, pb, memory_order_relaxed);
  atomic_store_explicit(&m->pub_voices, nvoices, memory_order_relaxed);
  atomic_store_explicit(&m->pub_streams, nstreams, memory_order_relaxed);
  atomic_fetch_add_explicit(&m->pub_blocks, 1, memory_order_relaxed);
  atomic_store_explicit(&m->pub_mixed, m->mixed, memory_order_release);
}

void cw__mixer_mix(float *out, int frames) {
  for (int off = 0; off < frames; off += CW_AUDIO_BLOCK) {
    int n = CW_MIN(CW_AUDIO_BLOCK, frames - off);
    if (cw__au.offline) cw__offline_prepare_streams();
    mix_block(out + off * 2, n);
  }
}

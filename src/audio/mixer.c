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
#include "cartwheel/profile.h"

static inline cw__mixer *M(void) { return &cw__au.mix; }

void cw__mixer_init(cw__mixer *m, int rate, int max_voices) {
  memset(m, 0, sizeof *m);
  m->rate = rate;
  m->max_voices = max_voices;
  m->voices = cw_calloc((size_t)max_voices, sizeof *m->voices, CW_MEM_AUDIO);
  m->clock_gen = cw_calloc((size_t)max_voices, sizeof *m->clock_gen, CW_MEM_AUDIO);
  m->clock = cw_calloc((size_t)max_voices, sizeof *m->clock, CW_MEM_AUDIO);
  m->started = cw_calloc((size_t)max_voices, sizeof *m->started, CW_MEM_AUDIO);
  m->scratch = cw_calloc(CW_AUDIO_BLOCK * 2, sizeof(float), CW_MEM_AUDIO);
  m->xa = cw_calloc(CW_AUDIO_BLOCK * 2, sizeof(float), CW_MEM_AUDIO);
  m->xb = cw_calloc(CW_AUDIO_BLOCK * 2, sizeof(float), CW_MEM_AUDIO);
  m->tres = cw_calloc((size_t)max_voices, sizeof *m->tres, CW_MEM_AUDIO);
  m->hstate = cw_calloc((size_t)max_voices, sizeof *m->hstate, CW_MEM_AUDIO);
  m->mono = cw_calloc(CW_AUDIO_BLOCK * 2, sizeof(float), CW_MEM_AUDIO);
  m->zone_in = cw_calloc(CW_AUDIO_BLOCK, sizeof(float), CW_MEM_AUDIO);
  m->dist = cw_calloc((size_t)max_voices, sizeof *m->dist, CW_MEM_AUDIO);
  m->hrtf_on = cw_calloc((size_t)max_voices, sizeof *m->hrtf_on, CW_MEM_AUDIO);
  m->hrtf_limit = 16;
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
  cw_free((void *)m->started);
  cw_free(m->scratch);
  cw_free(m->xa);
  cw_free(m->xb);
  cw_free(m->tres);
  cw_free(m->hstate);
  cw_free(m->mono);
  cw_free(m->zone_in);
  cw_free(m->dist);
  cw_free(m->hrtf_on);
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
  v->stop_frame = -1;
  v->priming = p->stream >= 0;
  v->air = 1.0f;
  if (p->d.fade_in > 0) {
    v->fade = 0;
    v->fade_target = 1;
    v->fade_step = 1.0f / (p->d.fade_in * (float)m->rate);
  } else {
    v->fade = v->fade_target = 1;
  }
  v->tempo = p->d.tempo;
  if (p->stream >= 0) {
    v->ipos = atomic_load_explicit(&cw__au.streams[p->stream].head, memory_order_acquire);
    v->played = (uint64_t)p->d.offset;
    v->udelta = (int64_t)v->played - (int64_t)v->ipos;
  } else {
    v->ipos = (uint64_t)p->d.offset;
    v->played = (uint64_t)p->d.offset;
  }
  atomic_store_explicit(&m->clock[c->slot], v->played, memory_order_release);
  atomic_store_explicit(&m->started[c->slot], -1, memory_order_release);
  atomic_store_explicit(&m->clock_gen[c->slot], v->gen, memory_order_release);
  cw__hrtf_reset(&m->hstate[c->slot]);
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
  case CW__CMD_STOP_AT:
    if (!(v = live_voice(c))) break;
    v->stop_frame = c->stop_at.frame < 0 ? 0 : c->stop_at.frame;
    v->stop_fade = c->stop_at.fade;
    break;
  case CW__CMD_PAUSE: if ((v = live_voice(c))) v->paused = c->f.flag; break;
  case CW__CMD_VOLUME: if ((v = live_voice(c))) v->volume = c->f.a; break;
  case CW__CMD_PITCH: if ((v = live_voice(c))) v->pitch = c->f.a; break;
  case CW__CMD_TEMPO:
  case CW__CMD_TEMPO_GROUP:
    for (int i = 0; i < c->tempo.n; i++) {
      cw__cmd one = { .slot = c->tempo.slot[i], .gen = c->tempo.gen[i] };
      if ((v = live_voice(&one))) v->tempo = c->tempo.tempo;
    }
    break;
  case CW__CMD_STRETCH: m->tres[c->slot] = c->tres; break;
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
  case CW__CMD_HRTF:
    if (m->hrtf) {
      cw__event e = { .type = CW__EV_HRTF_FREED, .hrtf = m->hrtf };
      cw__spsc_push(&cw__au.events, &e); /* room was checked before apply() */
    }
    m->hrtf = c->hrtf;
    for (int s = 0; s < m->max_voices; s++) m->hstate[s].valid = false; /* the old table's filters are gone */
    break;
  case CW__CMD_ZONE_ADD: {
    cw__mzone *z = &m->zones[c->slot];
    *z = (cw__mzone){ .used = true, .d = c->zone.desc, .fx = c->zone.fx };
    break;
  }
  case CW__CMD_ZONE_SET: {
    cw__mzone *z = &m->zones[c->slot];
    if (!z->used) break;
    z->d = c->zone.desc;
    cw__fdn_set(&z->fx->fdn, z->d.decay_s, z->d.hf_damp_hz, 1, 0);
    break;
  }
  case CW__CMD_ZONE_REMOVE: {
    cw__mzone *z = &m->zones[c->slot];
    if (!z->used) break;
    cw__event e = { .type = CW__EV_FX_FREED, .fx = z->fx };
    cw__spsc_push(&cw__au.events, &e); /* room was checked before apply() */
    *z = (cw__mzone){ 0 };
    atomic_store_explicit(&m->pub_zone_w[c->slot], 0, memory_order_relaxed);
    break;
  }
  case CW__CMD_OUTPUT: m->output = c->ival; break;
  case CW__CMD_HRTF_LIMIT: m->hrtf_limit = c->ival; break;
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

/* What voice_params found for a spatial voice besides the gains. */
typedef struct {
  float g;        /* gain before the pan */
  float pl, pr;   /* equal-power pan gains */
  float air;      /* distance low-pass cutoff factor (1 = none) */
  t3_vec3 local;  /* direction to the voice in its listener's space (x right, y up, -z forward), unit */
} vp_extra;

/* Per-block gains and pitch (including the 3D model). */
static void voice_params_x(cw__mixer *m, cw__mvoice *v, float fade_end, float *gl, float *gr, float *ratio, vp_extra *x) {
  float g = v->volume * fade_end;
  float pan = v->pan;
  float dop = 1.0f;
  if (x) {
    x->air = 1.0f;
    x->local = (t3_vec3){ 0, 0, -1 };
  }
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
    if (x) {
      if (s->air_absorption > 0) x->air = cw_pow(0.5f, s->air_absorption * (dc - ref));
      float fl = vlen(m->lfwd[li]);
      if (rl > 1e-6f && fl > 1e-6f && dist > 1e-6f) {
        t3_vec3 r = vscale(right, 1.0f / rl), f = vscale(m->lfwd[li], 1.0f / fl);
        t3_vec3 u = vcross(r, f); /* unit: r and f are orthonormal-ish; normalized below */
        float ul = vlen(u);
        if (ul > 1e-6f) u = vscale(u, 1.0f / ul);
        t3_vec3 l = { vdot(dn, r), vdot(dn, u), -vdot(dn, f) };
        float ll = vlen(l);
        if (ll > 1e-6f) x->local = vscale(l, 1.0f / ll);
      }
    }
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
    if (x) {
      x->pl = cs;
      x->pr = sn;
    }
  } else { /* stereo balance */
    *gl = g * (pan > 0 ? 1.0f - pan : 1.0f);
    *gr = g * (pan < 0 ? 1.0f + pan : 1.0f);
  }
  if (x) x->g = g;
  *ratio = cw_clampf(v->ratio * v->pitch * dop, 1.0f / 256.0f, 8.0f);
}

/* What the mixer still needs of a stream's ring: it reads on from `next`; `oldest` is the oldest frame it may
 * read again (what plays), kept with CW__STREAM_KEEP frames of history before it. */
static void stream_publish(cw__stream *st, uint64_t oldest, uint64_t next) {
  uint64_t r = oldest > CW__STREAM_KEEP ? oldest - CW__STREAM_KEEP : 0;
  if (r > atomic_load_explicit(&st->read, memory_order_relaxed)) /* never back: the producer may have written over it */
    atomic_store_explicit(&st->read, r, memory_order_release);
  atomic_store_explicit(&st->head, next, memory_order_release);
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
  bool lp = v->occlusion > 0 || v->air < 1.0f;
  float lpa = 1.0f;
  if (lp) {
    float fc = 20000.0f * (v->occlusion > 0 ? cw_pow(0.015f, v->occlusion) : 1.0f); /* 20 kHz .. 300 Hz */
    if (v->air < 1.0f) fc *= v->air;
    lpa = 1.0f - cw_exp(-CW_TAU * fc / (float)m->rate);
  }
  cw__stream *st = v->stream >= 0 ? &cw__au.streams[v->stream] : NULL;
  uint64_t write = 0, end = UINT64_MAX;
  uint32_t mask = 0;
  if (st) {
    write = atomic_load_explicit(&st->write, memory_order_acquire);
    end = atomic_load_explicit(&st->end, memory_order_acquire);
    mask = st->ring_frames - 1;
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
  if (st) stream_publish(st, v->ipos < write ? v->ipos : write, v->ipos < write ? v->ipos : write);
  return ended;
}

/* ── tempo ──────────────────────────────────────────────────────────────
 * A voice whose tempo is not 1 plays through its slot's stretcher (stretch.cpp): the source goes in, a signal at the
 * source's rate with the pitch kept comes out into the slot's ring of stretched frames, and the usual resampler
 * (pitch, the source rate, doppler) reads that ring as it reads a sample. Positions are integers and 32.32 fixed
 * point throughout, so every host takes the same source frames on the same blocks.
 *
 * Positions: u counts source frames as the clock does (unrolled through loops; for a stream, ring frames plus
 * udelta). The stretcher processes at tp, advancing exactly `tempo` source frames per stretched frame, and is fed
 * CW__STRETCH_IN_LAT frames past tp. Stretched frame q is heard as the source position the stretcher processed
 * CW__STRETCH_OUT_LAT stretched frames earlier: heard32(q), kept exact across tempo changes by the segment list. */

int cw__tempo_test;

static inline uint64_t tempo_step(float t) { return (uint64_t)((double)t * 4294967296.0 + 0.5); }

static void tseg_push(cw__mvoice *v, int64_t q0, int64_t p0, uint64_t step) {
  if (v->nseg == CW__TSEGS) { /* the oldest is long past what is heard */
    memmove(v->seg, v->seg + 1, (CW__TSEGS - 1) * sizeof *v->seg);
    v->nseg--;
  }
  v->seg[v->nseg++] = (cw__tseg){ q0, p0, step };
}

/* The source position (32.32) stretched frame q is heard as. */
static int64_t heard32(const cw__mvoice *v, int64_t q) {
  int64_t qh = q - CW__STRETCH_OUT_LAT;
  int i = v->nseg - 1;
  while (i > 0 && v->seg[i].q0 > qh) i--;
  const cw__tseg *g = &v->seg[i];
  return g->p0 + (qh - g->q0) * (int64_t)g->step;
}

/* The source frame a non-looping source ends on (unrolled), or -1 (it loops, or a stream not yet decoded to its end). */
static int64_t tempo_end(const cw__mvoice *v) {
  if (v->stream >= 0) {
    uint64_t end = atomic_load_explicit(&cw__au.streams[v->stream].end, memory_order_acquire);
    return end == UINT64_MAX ? -1 : (int64_t)end + v->udelta;
  }
  if (v->loop) return -1;
  return v->loop_end > 0 && v->loop_end <= v->frames ? v->loop_end : v->frames;
}

/* The bypass's own index (sample frame or ring frame) of unrolled source frame u. */
static uint64_t bypass_index(const cw__mvoice *v, int64_t u) {
  if (v->stream >= 0) return (uint64_t)(u - v->udelta);
  int64_t le = v->loop_end > 0 && v->loop_end <= v->frames ? v->loop_end : v->frames, len = le - v->loop_start;
  if (v->loop && u >= le && len > 0) u = v->loop_start + (u - le) % len;
  return (uint64_t)u;
}

/* Source frames [u, u + n) into the stretcher's input (planar; a mono source fills both channels). Before the start
 * or past the end: silence. False, reading nothing, when a stream's ring does not hold them yet. */
static bool src_read(const cw__mvoice *v, int64_t u, int n, float *L, float *R) {
  int ch = v->channels;
  if (v->stream >= 0) {
    cw__stream *st = &cw__au.streams[v->stream];
    uint64_t write = atomic_load_explicit(&st->write, memory_order_acquire);
    uint64_t end = atomic_load_explicit(&st->end, memory_order_acquire);
    int64_t i0 = u - v->udelta;
    if (end == UINT64_MAX && i0 + n > (int64_t)write) return false;
    int64_t oldest = (int64_t)write - (int64_t)st->ring_frames;
    uint32_t mask = st->ring_frames - 1;
    for (int k = 0; k < n; k++) {
      int64_t i = i0 + k;
      if (i < 0 || i < oldest || (uint64_t)i >= end) {
        L[k] = R[k] = 0;
        continue;
      }
      const float *p = st->ring + ((uint64_t)i & mask) * (uint32_t)ch;
      L[k] = p[0];
      R[k] = ch == 2 ? p[1] : p[0];
    }
    return true;
  }
  int64_t le = v->loop_end > 0 && v->loop_end <= v->frames ? v->loop_end : v->frames, len = le - v->loop_start;
  for (int k = 0; k < n; k++) {
    int64_t x = u + k;
    if (x >= le) x = v->loop && len > 0 ? v->loop_start + (x - le) % len : -1;
    if (x < 0) {
      L[k] = R[k] = 0;
      continue;
    }
    const float *p = v->pcm + x * ch;
    L[k] = p[0];
    R[k] = ch == 2 ? p[1] : p[0];
  }
  return true;
}

/* Runs the stretcher until the stretched frames before qto exist: at tempo 1 up to stretched frame
 * -CW__STRETCH_OUT_LAT (a warm up's lead-in), at the voice's tempo after it. A stream ring that does not hold the input
 * yet stops it (false) unless `silence` (a voice that is heard: the input is silence, counted as an underrun). */
static bool produce(cw__mixer *m, int slot, cw__mvoice *v, int64_t qto, bool silence) {
  cw__tres *tr = &m->tres[slot];
  float *L = cw__stretch_in(tr->st, 0), *R = cw__stretch_in(tr->st, 1);
  const float *oL = cw__stretch_out(tr->st, 0), *oR = cw__stretch_out(tr->st, 1);
  while (v->tq < qto) {
    int64_t k = CW_MIN(qto - v->tq, (int64_t)CW__STRETCH_CHUNK);
    uint64_t step = 1ull << 32;
    if (v->tq < -CW__STRETCH_OUT_LAT) k = CW_MIN(k, -CW__STRETCH_OUT_LAT - v->tq);
    else step = tempo_step(v->tempo);
    if (step != v->seg[v->nseg - 1].step) tseg_push(v, v->tq, v->tp, step);
    int64_t pe = v->tp + k * (int64_t)step;
    int64_t fe = (pe >> 32) + CW__STRETCH_IN_LAT;
    int nin = (int)(fe - v->tfed);
    if (!src_read(v, v->tfed, nin, L, R)) {
      if (!silence) return false;
      memset(L, 0, (size_t)nin * sizeof(float));
      memset(R, 0, (size_t)nin * sizeof(float));
      atomic_fetch_add_explicit(&m->pub_underruns, 1, memory_order_relaxed);
      atomic_fetch_add_explicit(&m->pub_underrun_frames, (uint32_t)k, memory_order_relaxed);
    }
    cw__stretch_process(tr->st, nin, (int)k);
    for (int64_t i = 0; i < k; i++) {
      uint32_t at = (uint32_t)((uint64_t)(v->tq + i) & (CW__TRING - 1));
      tr->ring[2 * at] = oL[i];
      tr->ring[2 * at + 1] = oR[i];
    }
    v->tq += k;
    v->tp = pe;
    v->tfed = fe;
  }
  return true;
}

/* Starts the stretcher so that stretched frame 0 is heard as source frame P: its history is the CW__STRETCH_SEEK
 * frames before P, and it then processes from P - CW__STRETCH_IN_LAT (stretched frame -(in + out latency)). False when
 * a stream's ring does not hold that history yet. */
static bool tempo_begin(cw__mixer *m, int slot, cw__mvoice *v, int64_t P) {
  cw__stretch *st = m->tres[slot].st;
  if (!src_read(v, P - CW__STRETCH_SEEK, CW__STRETCH_SEEK, cw__stretch_in(st, 0), cw__stretch_in(st, 1))) return false;
  cw__stretch_reset(st);
  cw__stretch_seek(st, CW__STRETCH_SEEK);
  v->tp = (P - CW__STRETCH_IN_LAT) * (int64_t)(1ll << 32);
  v->tfed = P;
  v->tq = -(CW__STRETCH_IN_LAT + CW__STRETCH_OUT_LAT);
  v->nseg = 0;
  tseg_push(v, v->tq, v->tp, 1ull << 32);
  v->tpe = P;
  return true;
}

/* Before a playing voice's block: moves it between the bypass and the stretched sound (see CW__T_*). */
static void tempo_prepare(cw__mixer *m, int slot, cw__mvoice *v, int n, float ratio) {
  bool want = (v->tempo != 1.0f || (cw__tempo_test & CW__TEMPO_TEST_FORCE)) && m->tres[slot].st;
  bool naive = cw__tempo_test & CW__TEMPO_TEST_NAIVE;
  switch (v->tmode) {
  case CW__T_OFF:
    if (!want) break;
    if (!v->begun) { /* its first block: stretched from its first frame, pre-fed */
      if (tempo_begin(m, slot, v, (int64_t)v->played)) {
        produce(m, slot, v, 0, true);
        v->sidx = 0;
        v->sfrac = v->frac;
        v->tlp[0] = v->lp[0];
        v->tlp[1] = v->lp[1];
        v->tmode = CW__T_ON;
      }
      break;
    } else if (naive) { /* the control: started where it is, its pre-roll heard, no cross-fade */
      if (!tempo_begin(m, slot, v, (int64_t)v->played)) break;
      v->tq = 0;
      v->sidx = 0;
      v->sfrac = v->frac;
      v->tmode = CW__T_ON;
      break;
    } else { /* playing at tempo 1: warm up for where it will be CW__TEMPO_ENTER mixer frames from now */
      uint64_t step = (uint64_t)((double)ratio * 4294967296.0);
      int64_t P = (int64_t)v->played + (int64_t)(((uint64_t)v->frac + (uint64_t)CW__TEMPO_ENTER * step) >> 32);
      if (!tempo_begin(m, slot, v, P)) break;
      v->twarm = 0;
      v->tmode = CW__T_WARM;
    }
    /* fallthrough */
  case CW__T_WARM: {
    if (!want) {
      v->tmode = CW__T_OFF;
      break;
    }
    if (v->twarm < CW__TEMPO_ENTER || v->tq < 0) { /* the warm up, spread over CW__TEMPO_ENTER mixer frames */
      v->twarm = (uint32_t)CW_MIN((int64_t)v->twarm + n, (int64_t)CW__TEMPO_ENTER);
      const int64_t total = CW__STRETCH_IN_LAT + CW__STRETCH_OUT_LAT;
      produce(m, slot, v, -total + total * (int64_t)v->twarm / CW__TEMPO_ENTER, false);
      break;
    }
    /* warm, and the bypass has reached the stretched sound's frame 0: cross-fade in on the stretched frame heard as
     * where the bypass is now */
    int64_t x = (int64_t)v->played * (int64_t)(1ll << 32) + (int64_t)v->frac;
    int i = v->nseg - 1;
    double q = 0;
    for (; i >= 0; i--) {
      const cw__tseg *g = &v->seg[i];
      q = (double)(x - g->p0) / (double)g->step + (double)g->q0 + CW__STRETCH_OUT_LAT;
      if (q - CW__STRETCH_OUT_LAT >= (double)g->q0 || i == 0) break;
    }
    if (q < 0) q = 0; /* the bypass is behind (its pitch fell): start at frame 0 */
    v->sidx = (int64_t)q;
    v->sfrac = (uint32_t)((q - (double)v->sidx) * 4294967296.0);
    v->tlp[0] = v->lp[0];
    v->tlp[1] = v->lp[1];
    v->tmode = CW__T_IN;
    break;
  }
  case CW__T_ON: {
    while (v->nseg > 1 && v->seg[1].q0 <= v->sidx - CW__STRETCH_OUT_LAT) { /* only the segment heard now matters */
      memmove(v->seg, v->seg + 1, (size_t)(v->nseg - 1) * sizeof *v->seg);
      v->nseg--;
    }
    const cw__tseg *g = &v->seg[v->nseg - 1];
    if (!want && g->step == 1ull << 32 && v->sidx - CW__STRETCH_OUT_LAT >= g->q0) {
      /* tempo 1 is what is heard now: cross-fade back to the source itself, at the same place */
      int64_t x = heard32(v, v->sidx) + (int64_t)v->sfrac;
      if (x < 0) x = 0;
      v->played = (uint64_t)(x >> 32);
      v->frac = (uint32_t)x;
      v->ipos = bypass_index(v, (int64_t)v->played);
      v->lp[0] = v->tlp[0];
      v->lp[1] = v->tlp[1];
      v->tmode = naive ? CW__T_OFF : CW__T_OUT;
    }
    break;
  }
  default: break;
  }
}

/* The stretched path: like render_voice, reading the slot's ring of stretched frames. Returns true when the source
 * (not looping) ended inside; the clock becomes the source position heard. */
static bool render_stretched(cw__mixer *m, int slot, cw__mvoice *v, float *out, int n, float gl0, float gr0, float gl1,
                             float gr1, float ratio) {
  uint64_t step = (uint64_t)((double)ratio * 4294967296.0);
  uint32_t step_lo = (uint32_t)step;
  uint64_t step_hi = step >> 32;
  bool mono_mix = v->spatial && v->channels == 2;
  float dgl = (gl1 - gl0) / (float)n, dgr = (gr1 - gr0) / (float)n;
  float gl = gl0, gr = gr0;
  bool lp = v->occlusion > 0 || v->air < 1.0f;
  float lpa = 1.0f;
  if (lp) {
    float fc = 20000.0f * (v->occlusion > 0 ? cw_pow(0.015f, v->occlusion) : 1.0f);
    if (v->air < 1.0f) fc *= v->air;
    lpa = 1.0f - cw_exp(-CW_TAU * fc / (float)m->rate);
  }
  int64_t last = v->sidx + (int64_t)(((uint64_t)v->sfrac + (uint64_t)n * step) >> 32);
  produce(m, slot, v, last + 2, true);
  int64_t end = tempo_end(v), qend = INT64_MAX;
  if (end >= 0 && (heard32(v, last + 1) >> 32) >= end) { /* it ends in this block: on the frame heard as its end */
    qend = v->sidx;
    while (qend <= last + 1 && (heard32(v, qend) >> 32) < end) qend++;
  }
  const float *ring = m->tres[slot].ring;
  const uint32_t mask = CW__TRING - 1;
  bool ended = false;
  for (int i = 0; i < n; i++) {
    if (v->sidx >= qend) {
      ended = true;
      break;
    }
    const float *pa = ring + 2 * ((uint64_t)v->sidx & mask), *pb = ring + 2 * ((uint64_t)(v->sidx + 1) & mask);
    float t = (float)(v->sfrac >> 8) * (1.0f / 16777216.0f);
    float s0 = pa[0] + (pb[0] - pa[0]) * t, s1 = pa[1] + (pb[1] - pa[1]) * t;
    if (mono_mix) s0 = s1 = (s0 + s1) * 0.5f;
    if (lp) {
      v->tlp[0] += lpa * (s0 - v->tlp[0]);
      v->tlp[1] += lpa * (s1 - v->tlp[1]);
      s0 = v->tlp[0];
      s1 = v->tlp[1];
    } else {
      v->tlp[0] = s0;
      v->tlp[1] = s1;
    }
    gl += dgl;
    gr += dgr;
    out[2 * i] += s0 * gl;
    out[2 * i + 1] += s1 * gr;
    uint32_t f = v->sfrac + step_lo;
    v->sidx += (int64_t)(step_hi + (f < v->sfrac ? 1 : 0));
    v->sfrac = f;
  }
  int64_t h = heard32(v, v->sidx) >> 32;
  v->played = h > 0 ? (uint64_t)h : 0;
  if (v->stream >= 0) { /* the ring holds what is heard (with its history) up to what is fed */
    int64_t oldest = (int64_t)v->played - v->udelta, next = v->tfed - v->udelta;
    stream_publish(&cw__au.streams[v->stream], oldest > 0 ? (uint64_t)oldest : 0, next > 0 ? (uint64_t)next : 0);
  }
  return ended;
}

/* One voice's block, through the bypass, the stretcher, or (IN, OUT) both cross-faded over the block. */
static bool render_any(cw__mixer *m, int slot, cw__mvoice *v, float *out, int n, float gl0, float gr0, float gl1,
                       float gr1, float ratio) {
  switch (v->tmode) {
  case CW__T_ON: return render_stretched(m, slot, v, out, n, gl0, gr0, gl1, gr1, ratio);
  case CW__T_IN:
  case CW__T_OUT: {
    float *a = m->xa, *b = m->xb;
    memset(a, 0, (size_t)n * 2 * sizeof(float));
    memset(b, 0, (size_t)n * 2 * sizeof(float));
    bool in = v->tmode == CW__T_IN;
    bool ea = render_voice(m, v, a, n, gl0, gr0, gl1, gr1, ratio);
    uint64_t played = v->played;
    bool eb = render_stretched(m, slot, v, b, n, gl0, gr0, gl1, gr1, ratio);
    float inv = 1.0f / (float)n;
    for (int i = 0; i < n; i++) {
      float w = (float)(i + 1) * inv; /* the stretched share: rising in, falling out */
      if (!in) w = 1.0f - w;
      out[2 * i] += a[2 * i] + (b[2 * i] - a[2 * i]) * w;
      out[2 * i + 1] += a[2 * i + 1] + (b[2 * i + 1] - a[2 * i + 1]) * w;
    }
    if (in) {
      v->tmode = CW__T_ON;
      return eb;
    }
    v->played = played; /* the bypass from here */
    v->tmode = CW__T_OFF;
    if (v->stream >= 0) stream_publish(&cw__au.streams[v->stream], v->ipos, v->ipos);
    return ea;
  }
  default: return render_voice(m, v, out, n, gl0, gr0, gl1, gr1, ratio);
  }
}

/* ── HRTF selection ─────────────────────────────────────────────────── */

/* Headphones mode: the hrtf_limit spatial voices nearest their listener get
 * the HRTF this block (ties: the lower slot). */
static void select_hrtf(cw__mixer *m, uint64_t F, int n) {
  memset(m->hrtf_on, 0, (size_t)m->max_voices);
  if (m->output != CW_AUDIO_HEADPHONES || !m->hrtf || m->hrtf_limit <= 0) return;
  int cands = 0;
  for (int s = 0; s < m->max_voices; s++) {
    const cw__mvoice *v = &m->voices[s];
    m->dist[s] = -1;
    bool live = v->state == CW__V_PLAYING || (v->state == CW__V_WAITING && v->start_frame < (int64_t)(F + (uint64_t)n));
    if (!live || v->paused || !v->spatial || v->sp.mode == CW_SPATIAL_PAN) continue;
    float best = 0;
    for (int l = 0; l < m->nlisteners; l++) {
      float d = vlen(vsub(v->sp.position, m->lpos[l]));
      if (l == 0 || d < best) best = d;
    }
    m->dist[s] = best;
    cands++;
  }
  if (cands <= m->hrtf_limit) {
    for (int s = 0; s < m->max_voices; s++) m->hrtf_on[s] = m->dist[s] >= 0;
    return;
  }
  for (int k = 0; k < m->hrtf_limit; k++) {
    int pick = -1;
    for (int s = 0; s < m->max_voices; s++)
      if (m->dist[s] >= 0 && !m->hrtf_on[s] && (pick < 0 || m->dist[s] < m->dist[pick])) pick = s;
    if (pick < 0) break;
    m->hrtf_on[pick] = 1;
  }
}

/* ── reverb zones ───────────────────────────────────────────────────── */

/* How much listener 0 is in zone z (before priorities): 1 inside, fading to
 * 0 over blend outside. */
static float zone_presence(const cw__mixer *m, const cw_reverb_zone_desc *d) {
  t3_vec3 p = vsub(m->lpos[0], d->center);
  float out;
  if (d->shape == CW_ZONE_SPHERE) {
    out = vlen(p) - d->radius;
  } else {
    t3_vec3 q = { (p.x < 0 ? -p.x : p.x) - d->half_extents.x, (p.y < 0 ? -p.y : p.y) - d->half_extents.y,
                  (p.z < 0 ? -p.z : p.z) - d->half_extents.z };
    t3_vec3 o = { q.x > 0 ? q.x : 0, q.y > 0 ? q.y : 0, q.z > 0 ? q.z : 0 };
    out = vlen(o);
  }
  if (out <= 0) return 1.0f;
  return d->blend > 0 ? cw_clampf(1.0f - out / d->blend, 0, 1) : 0.0f;
}

/* Shares this block: higher priority first (ties: lower index), each taking
 * its presence times what is left. */
static void zone_weights(const cw__mixer *m, float *w) {
  float left = 1.0f;
  bool done[CW_AUDIO_MAX_REVERB_ZONES] = { 0 };
  for (int k = 0; k < CW_AUDIO_MAX_REVERB_ZONES; k++) {
    int pick = -1;
    for (int z = 0; z < CW_AUDIO_MAX_REVERB_ZONES; z++)
      if (m->zones[z].used && !done[z] && (pick < 0 || m->zones[z].d.priority > m->zones[pick].d.priority)) pick = z;
    if (pick < 0) break;
    done[pick] = true;
    float s = zone_presence(m, &m->zones[pick].d) * left;
    w[pick] = s;
    left -= s;
  }
}

static float voice_send(const cw__mvoice *v) {
  if (!v->spatial || v->sp.reverb_send < 0) return 0.0f;
  return v->sp.reverb_send > 0 ? v->sp.reverb_send : 1.0f;
}

/* ── one block ──────────────────────────────────────────────────────── */

static void mix_block(float *out, int n) {
  cw__mixer *m = M();
  uint64_t F = m->mixed;
  drain_commands();
  for (int b = 0; b < CW_AUDIO_MAX_BUSES; b++)
    if (m->buses[b].used) memset(m->buses[b].buf, 0, (size_t)n * 2 * sizeof(float));
  int nvoices = 0, nstreams = 0, nhrtf = 0;
  select_hrtf(m, F, n);
  /* reverb zones: this block's shares; sends are gathered only when one listens */
  float zw[CW_AUDIO_MAX_REVERB_ZONES] = { 0 };
  zone_weights(m, zw);
  bool zones_feed = false;
  for (int z = 0; z < CW_AUDIO_MAX_REVERB_ZONES; z++) zones_feed |= m->zones[z].used && (zw[z] > 0 || m->zones[z].w > 0);
  memset(m->zone_in, 0, (size_t)n * sizeof(float));
  for (int s = 0; s < m->max_voices; s++) {
    cw__mvoice *v = &m->voices[s];
    if (v->state == CW__V_ZOMBIE) { end_voice(s); continue; }
    if (v->state == CW__V_FREE) continue;
    int off = 0;
    if (v->state == CW__V_WAITING) {
      if (v->start_frame >= (int64_t)(F + (uint64_t)n)) continue;
      if (v->start_frame > (int64_t)F) off = (int)(v->start_frame - (int64_t)F);
      v->state = CW__V_PLAYING;
      atomic_store_explicit(&m->started[s], (int64_t)F + off, memory_order_release);
    }
    if (v->paused) continue;
    if (v->priming) {
      cw__stream *st = &cw__au.streams[v->stream];
      uint64_t have = atomic_load_explicit(&st->write, memory_order_acquire) - v->ipos;
      bool ended = atomic_load_explicit(&st->end, memory_order_acquire) != UINT64_MAX;
      uint64_t want = CW_MIN((uint64_t)st->cap / 2, (uint64_t)2048); /* ~43 ms buffered */
      if (v->tempo != 1.0f) /* a stretched start reads its look-ahead at once */
        want = CW_MIN((uint64_t)st->cap * 3 / 4, (uint64_t)(CW__STRETCH_IN_LAT + 2 * CW__STRETCH_OUT_LAT + 2048));
      if (!ended && have < want) continue;
      v->priming = false;
    }
    int cnt = n - off;
    /* an end on a given frame (music switching on a beat): a hard stop renders up to it and ends; a fading one starts
     * its fade in the block that holds the frame */
    bool stop_here = false;
    if (v->stop_frame >= 0 && v->stop_frame < (int64_t)(F + (uint64_t)n)) {
      if (v->stop_fade > 0) {
        v->fade_target = 0;
        v->fade_step = 1.0f / (v->stop_fade * (float)m->rate);
        v->stop_at_fade_end = true;
        v->stop_frame = -1;
      } else {
        int64_t lim = v->stop_frame - (int64_t)F - off;
        if (lim <= 0) { end_voice(s); continue; }
        if (lim < cnt) cnt = (int)lim;
        stop_here = true;
      }
    }
    /* fade across this block */
    float fade_end = v->fade;
    if (v->fade_step > 0) {
      float d = v->fade_step * (float)cnt;
      fade_end = v->fade < v->fade_target ? CW_MIN(v->fade + d, v->fade_target) : CW_MAX(v->fade - d, v->fade_target);
    }
    float gl1, gr1, ratio;
    vp_extra x1, x0;
    voice_params_x(m, v, fade_end, &gl1, &gr1, &ratio, &x1);
    float gl0 = gl1, gr0 = gr1;
    x0 = x1;
    if (v->gains_valid) {
      gl0 = v->gl;
      gr0 = v->gr;
      x0.g = v->g;
      x0.pl = v->pl;
      x0.pr = v->pr;
    } else if (v->fade_step > 0 || fade_end != v->fade) { /* first block of a fade: ramp from the start level */
      voice_params_x(m, v, v->fade, &gl0, &gr0, &ratio, &x0);
      voice_params_x(m, v, fade_end, &gl1, &gr1, &ratio, &x1);
    }
    v->air = x1.air;
    int bus = v->bus >= 0 && v->bus < CW_AUDIO_MAX_BUSES && m->buses[v->bus].used ? v->bus : 0;
    float *dst = m->buses[bus].buf + off * 2;
    /* HRTF share: ramps toward 1 for a selected voice, 0 otherwise; a voice's
     * first block starts at its target */
    cw__hstate *hs = &m->hstate[s];
    float h1 = v->spatial && m->hrtf_on[s] ? 1.0f : 0.0f;
    float h0 = v->gains_valid ? hs->mix : h1;
    bool ended;
    if (v->tempo != 1.0f || v->tmode != CW__T_OFF || cw__tempo_test) tempo_prepare(m, s, v, cnt, ratio);
    float send = zones_feed ? voice_send(v) : 0.0f;
    if (h0 == 0 && h1 == 0 && send == 0) {
      ended = render_any(m, s, v, dst, cnt, gl0, gr0, gl1, gr1, ratio);
    } else {
      /* the gained mono signal, then the HRTF and (while crossfading) the pan */
      float *mono = m->mono;
      memset(mono, 0, (size_t)cnt * 2 * sizeof(float));
      ended = render_any(m, s, v, mono, cnt, x0.g, x0.g, x1.g, x1.g, ratio);
      if (send > 0) {
        float *zi = m->zone_in + off;
        for (int i = 0; i < cnt; i++) zi[i] += mono[2 * i] * send;
      }
      if (h0 > 0 || h1 > 0) {
        if (h0 == 0) { /* entering: the history is stale */
          memset(hs->hist, 0, sizeof hs->hist);
          hs->valid = false;
        }
        float *in = hs->hist + CW__HRTF_PRE;
        for (int i = 0; i < cnt; i++) in[i] = mono[2 * i];
        cw__hrtf_set_direction(m->hrtf, hs, x1.local);
        cw__hrtf_render(hs, cnt, dst, h0, h1);
      }
      if (h0 < 1 || h1 < 1) {
        float inv = 1.0f / (float)cnt;
        for (int i = 0; i < cnt; i++) {
          float t = (float)(i + 1) * inv;
          float pan_share = 1.0f - (h0 + (h1 - h0) * t);
          float pl = x0.pl + (x1.pl - x0.pl) * t, pr = x0.pr + (x1.pr - x0.pr) * t;
          dst[2 * i] += mono[2 * i] * pl * pan_share;
          dst[2 * i + 1] += mono[2 * i] * pr * pan_share;
        }
      }
    }
    hs->mix = h1;
    v->gl = gl1;
    v->gr = gr1;
    v->g = x1.g;
    v->pl = x1.pl;
    v->pr = x1.pr;
    v->gains_valid = true;
    v->begun = true;
    v->fade = fade_end;
    if (v->fade == v->fade_target) v->fade_step = 0;
    atomic_store_explicit(&m->clock[s], v->played, memory_order_release);
    if (ended || stop_here || (v->stop_at_fade_end && v->fade <= 0)) end_voice(s);
  }
  for (int s = 0; s < m->max_voices; s++) {
    const cw__mvoice *v = &m->voices[s];
    if (v->state != CW__V_WAITING && v->state != CW__V_PLAYING) continue;
    nvoices++;
    nstreams += v->stream >= 0;
    nhrtf += m->hrtf_on[s];
  }
  /* reverb zones play into their buses before the buses run */
  for (int z = 0; z < CW_AUDIO_MAX_REVERB_ZONES; z++) {
    cw__mzone *zn = &m->zones[z];
    if (!zn->used) continue;
    float w0 = zn->w, w1 = zw[z];
    bool fed = zones_feed && (w0 > 0 || w1 > 0);
    if (fed || zn->ringing) {
      float level = zn->d.level > 0 ? zn->d.level : 1.0f;
      int zb = zn->d.bus > 0 && zn->d.bus < CW_AUDIO_MAX_BUSES && m->buses[zn->d.bus].used ? zn->d.bus : 0;
      /* not fed: input gains 0 (the tail alone) */
      float pk = cw__fdn_process_mono(&zn->fx->fdn, m->zone_in, fed ? w0 * level : 0.0f, fed ? w1 * level : 0.0f, n, m->buses[zb].buf);
      /* quiet for long enough to empty the pre-delay: stop running it */
      zn->quiet = fed || pk > 1e-6f ? 0 : zn->quiet + n;
      zn->ringing = zn->quiet <= zn->fx->fdn.pre_len + n;
      if (!zn->ringing) cw__fdn_clear(&zn->fx->fdn);
    }
    zn->w = w1;
    uint32_t wb;
    memcpy(&wb, &w1, 4);
    atomic_store_explicit(&m->pub_zone_w[z], wb, memory_order_relaxed);
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
  atomic_store_explicit(&m->pub_hrtf, nhrtf, memory_order_relaxed);
  atomic_fetch_add_explicit(&m->pub_blocks, 1, memory_order_relaxed);
  atomic_store_explicit(&m->pub_mixed, m->mixed, memory_order_release);
}

void cw__mixer_mix(float *out, int frames) {
  CW_PROFILE_SCOPE("audio.mix");
  for (int off = 0; off < frames; off += CW_AUDIO_BLOCK) {
    int n = CW_MIN(CW_AUDIO_BLOCK, frames - off);
    if (cw__au.offline) cw__offline_prepare_streams();
    mix_block(out + off * 2, n);
  }
}

/* Audio player and listener nodes (cartwheel/audio_nodes.h) over the mixer (cartwheel/audio.h).
 *
 * Every tree with one of these nodes has a state (tree slot "audio_nodes") listing its players and listeners
 * and one fixed-phase hook after the physics step that sets the listeners, moves spatial voices, holds and
 * resumes voices with the tree's pause, and counts each voice toward `finished` in fixed ticks. */
#include <math.h>
#include <string.h>

#include "cartwheel/audio_nodes.h"
#include "cartwheel/math.h"
#include "cartwheel/render3d.h"
#include "cartwheel/scene2d.h"

#define MAX_POLY 8
enum { P_PLAIN, P_2D, P_3D };

typedef struct player {
  const char *stream; /* interned asset path */
  float volume_db, pitch_scale;
  bool autoplay, loop;
  const char *bus;
  int max_polyphony;
  bool stream_paused;
  /* 2D */
  float max_distance2d, attenuation, panning_strength;
  /* 3D */
  int attenuation_model;
  float unit_size, max_distance, rolloff, cone_inner_angle, cone_outer_angle, cone_outer_gain, doppler, air_absorption,
    reverb_send, occlusion;
  bool panning_only;
  /* runtime */
  int kind;
  bool in_tree, want_autoplay, held; /* held: the voices are paused (stream_paused or the tree's pause) */
  int nvoices;
  cw_voice voices[MAX_POLY]; /* oldest first */
  double played[MAX_POLY];   /* source frames played, counted in fixed ticks */
  int64_t length[MAX_POLY];  /* source frames; -1 unknown */
  int rate[MAX_POLY];        /* source frames a second */
  cw_sound sound;            /* a sound given in code (cw_audio_player_set_sound): played instead of stream */
  const char *clip;          /* an interactive stream's clip to play (interned; "" = its initial clip) */
  const cw_audio_stream_res *res; /* stream is a .cwaudio resource: a randomizer or an interactive music */
  const char *res_path;
  cw_music music;            /* this player's own music of an interactive resource */
  bool music_on;
  bool have_prev;
  t3_vec3 prev_pos;          /* 3D: last tick's world position (velocity) */
  float gain2d, pan2d;       /* 2D: what the voices were last given */
} player;

typedef struct listener {
  bool current;
  bool have_prev;
  t3_vec3 prev_pos;
} listener;

typedef struct tree_state {
  CW_ARRAY(cw_node) players;
  CW_ARRAY(cw_node) listeners;
  bool hooked, auto_listener, own_audio;
  bool cam_prev;
  t3_vec3 cam_prev_pos;
  uint64_t cam_id;
} tree_state;

static cw_node_type player_type, player2d_type, player3d_type, listener_type;
static const char *s_finished;
static cw_map sounds; /* interned path pointer -> cw_sound id */
static bool sounds_init;

static const char *const atten_names[] = { "NONE", "INVERSE", "LINEAR", "EXPONENTIAL", NULL };

static player *player_of(cw_node n) {
  player *p = cw_node_data_t(n, &player_type);
  if (!p) p = cw_node_data_t(n, &player2d_type);
  if (!p) p = cw_node_data_t(n, &player3d_type);
  return p;
}

static void state_free(void *v) {
  tree_state *s = v;
  cw_array_free(&s->players);
  cw_array_free(&s->listeners);
  cw_free(s);
}
static void tick_hook(cw_tree *t, double dt, void *user);
static void frame_hook(cw_tree *t, double dt, void *user);
static tree_state *state_of(cw_tree *t, bool create) {
  tree_state *s = cw_tree_slot(t, "audio_nodes");
  if (!s && create && t) {
    s = cw_calloc(1, sizeof *s, CW_MEM_SCENE);
    s->auto_listener = true;
    cw_tree_set_slot(t, "audio_nodes", s, state_free);
  }
  if (s && !s->hooked) {
    s->hooked = true;
    cw_tree_add_hook(t, 0, 150, tick_hook, NULL); /* after physics (100), before Camera2D smoothing (200) */
    cw_tree_add_hook(t, 1, 150, frame_hook, NULL);
  }
  return s;
}
static void list_remove(void *arr, cw_node n) {
  CW_ARRAY(cw_node) *a = arr;
  for (int i = 0; i < a->count; i++)
    if (a->data[i].id == n.id) {
      memmove(a->data + i, a->data + i + 1, sizeof(cw_node) * (size_t)(a->count - i - 1));
      a->count--;
      return;
    }
}

void cw_audio_nodes_set_auto_listener(cw_tree *t, bool on) {
  tree_state *s = state_of(t, true);
  if (s) s->auto_listener = on;
}

/* ── sounds ─────────────────────────────────────────────────────────── */

/* The mixer, started with the defaults when nobody has; the tree then pumps cw_audio_update. */
static bool ensure_audio(cw_tree *t) {
  if (cw_audio_sample_rate() > 0) return true;
  if (!cw_audio_init(NULL)) return false;
  tree_state *s = state_of(t, true);
  if (s) s->own_audio = true;
  return cw_audio_sample_rate() > 0;
}

static cw_sound sound_for(const char *path) {
  if (!path || !*path) return (cw_sound){ 0 };
  if (!sounds_init) {
    cw_map_init(&sounds, CW_MEM_SCENE); /* lives for the program: not the mixer's memory, which a shutdown returns */
    sounds_init = true;
  }
  uint64_t key = (uint64_t)(uintptr_t)path, v;
  if (cw_map_get(&sounds, key, &v)) {
    cw_sound s = { v };
    if (cw_sound_source_rate(s) > 0) return s; /* still alive (the mixer was not shut down since) */
  }
  cw_sound s = cw_sound_load(path, NULL);
  if (CW_HANDLE_IS_NULL(s)) {
    cw_map_remove(&sounds, key);
    return s;
  }
  cw_map_set(&sounds, key, s.id);
  return s;
}

static float db_gain(float db) {
  if (!(db > -80)) return 0;
  return cw_exp(db * 0.115129255f); /* 10 ^ (db / 20) */
}

/* ── placing voices ─────────────────────────────────────────────────── */

static cw_voice_3d space_of(cw_node n, const player *p, t3_vec3 vel) {
  const t3_mat4 *m = cw_node3d_global_transform(n);
  cw_voice_3d sp = { 0 };
  if (m) {
    sp.position = (t3_vec3){ m->e[12], m->e[13], m->e[14] };
    bool cone = p->cone_inner_angle < 360 || p->cone_outer_angle < 360;
    if (cone) sp.direction = (t3_vec3){ -m->e[8], -m->e[9], -m->e[10] }; /* the node's -Z */
  }
  sp.velocity = vel;
  sp.model = (cw_atten_model)p->attenuation_model;
  sp.ref_distance = p->unit_size;
  sp.max_distance = p->max_distance;
  sp.rolloff = p->rolloff;
  sp.cone_inner_deg = p->cone_inner_angle;
  sp.cone_outer_deg = p->cone_outer_angle;
  sp.cone_outer_gain = p->cone_outer_gain;
  sp.doppler = p->doppler;
  sp.mode = p->panning_only ? CW_SPATIAL_PAN : CW_SPATIAL_AUTO;
  sp.air_absorption = p->air_absorption;
  sp.reverb_send = p->reverb_send > 0 ? p->reverb_send : -1;
  return sp;
}

/* the 2D player's gain and pan from where it is on the last drawn view */
static void place2d(cw_node n, player *p) {
  p->gain2d = 1;
  p->pan2d = 0;
  cw_tree *t = cw_node_tree(n);
  cw_rect vp;
  if (!t || !cw_canvas_viewport(t, &vp) || !(vp.w > 0)) return;
  cw_vec2 pos = cw_node2d_global_position(n);
  cw_vec2 mid = cw_canvas_screen_to_world(t, cw_v2(vp.x + vp.w * 0.5f, vp.y + vp.h * 0.5f));
  float d = cw_v2_length(cw_v2_sub(pos, mid));
  if (p->max_distance2d > 0) {
    float k = 1 - d / p->max_distance2d;
    p->gain2d = k <= 0 ? 0 : p->attenuation == 1 ? k : cw_pow(k, p->attenuation);
  }
  cw_vec2 scr = cw_canvas_world_to_screen(t, pos);
  p->pan2d = cw_clampf((scr.x - (vp.x + vp.w * 0.5f)) / (vp.w * 0.5f), -1, 1) * p->panning_strength;
}

static float voice_gain(const player *p) { return db_gain(p->volume_db) * (p->kind == P_2D ? p->gain2d : 1); }

/* volume 0 means "the sound's default" to the mixer's play call, so silence is set after it */
static void apply_gain(const player *p, cw_voice v) { cw_voice_set_volume(v, voice_gain(p)); }

/* ── play / stop ────────────────────────────────────────────────────── */

static void drop_voice(player *p, int i) {
  for (int k = i; k + 1 < p->nvoices; k++) {
    p->voices[k] = p->voices[k + 1];
    p->played[k] = p->played[k + 1];
    p->length[k] = p->length[k + 1];
    p->rate[k] = p->rate[k + 1];
  }
  p->nvoices--;
}

static bool effective_hold(cw_node n, const player *p) {
  return p->stream_paused || (p->in_tree && !cw_node_can_process(n));
}

static bool is_res_path(const char *path) {
  size_t L = path ? strlen(path) : 0;
  return L > 8 && !strcmp(path + L - 8, ".cwaudio");
}
/* the player's .cwaudio resource, loaded once per stream path */
static const cw_audio_stream_res *res_of(player *p) {
  if (!is_res_path(p->stream)) return NULL;
  if (p->res_path != p->stream) {
    p->res = cw_audio_stream_load(p->stream);
    p->res_path = p->stream;
  }
  return p->res;
}
static cw_music_desc music_desc(const player *p) {
  int bus = p->bus && *p->bus ? cw_bus_find(p->bus) : 0;
  float g = voice_gain(p);
  return (cw_music_desc){ .bus = bus > 0 ? bus : 0, .volume = g > 0 ? g : 0.00001f };
}

cw_voice cw_audio_player_play(cw_node n, double from_seconds) {
  player *p = player_of(n);
  if (!p) return (cw_voice){ 0 };
  bool given = !CW_HANDLE_IS_NULL(p->sound);
  if (!given && (!p->stream || !*p->stream)) {
    char path[256];
    cw_node_path(n, (cw_node){ 0 }, path, sizeof path);
    cw_warn("%s: play with no stream (set stream to a sound's asset path, or give it a sound with cw_audio_player_set_sound)", path);
    return (cw_voice){ 0 };
  }
  if (!ensure_audio(cw_node_tree(n))) return (cw_voice){ 0 };
  const cw_audio_stream_res *res = given ? NULL : res_of(p);
  if (!given && is_res_path(p->stream) && !res) return (cw_voice){ 0 };
  if (res && cw_audio_stream_is_interactive(res)) { /* music: the player's own, started (or switched) to its clip */
    if (CW_HANDLE_IS_NULL(p->music)) {
      cw_music_desc md = music_desc(p);
      p->music = cw_audio_stream_music(res, &md);
    }
    if (p->kind == P_3D) {
      cw_voice_3d sp = space_of(n, p, (t3_vec3){ 0, 0, 0 });
      cw_music_set_3d(p->music, &sp);
    }
    cw_music_set_paused(p->music, effective_hold(n, p));
    p->held = effective_hold(n, p);
    cw_music_play(p->music, p->clip && *p->clip ? p->clip : cw_audio_stream_initial(res));
    p->music_on = true;
    return cw_music_voice(p->music);
  }
  float rnd_pitch = 1, rnd_gain = 1;
  cw_sound s = given ? p->sound : res ? cw_randomizer_next(cw_audio_stream_randomizer(res), &rnd_pitch, &rnd_gain) : sound_for(p->stream);
  if (CW_HANDLE_IS_NULL(s)) return (cw_voice){ 0 };
  int poly = p->max_polyphony < 1 ? 1 : p->max_polyphony > MAX_POLY ? MAX_POLY : p->max_polyphony;
  while (p->nvoices >= poly) {
    cw_voice_stop(p->voices[0], 0);
    drop_voice(p, 0);
  }
  int rate = cw_sound_source_rate(s);
  if (!(from_seconds > 0)) from_seconds = 0;
  cw_play_desc d = { .pitch = (p->pitch_scale > 0 ? p->pitch_scale : 1) * rnd_pitch, .loop = p->loop, .offset = (int64_t)(from_seconds * rate),
                     .paused = effective_hold(n, p) };
  int bus = p->bus && *p->bus ? cw_bus_find(p->bus) : 0;
  if (bus < 0 && p->bus && (!strcmp(p->bus, "Master") || !strcmp(p->bus, "master"))) bus = 0;
  d.bus = bus > 0 ? bus : 0;
  if (p->kind == P_2D && p->in_tree) {
    place2d(n, p);
    d.pan = p->pan2d;
  } else if (p->kind == P_3D) {
    d.spatial = true;
    d.space = space_of(n, p, (t3_vec3){ 0, 0, 0 });
  }
  float g = voice_gain(p) * rnd_gain;
  d.volume = g > 0 ? g : 1;
  cw_voice v = cw_audio_play(s, &d);
  if (CW_HANDLE_IS_NULL(v)) return v;
  if (!(g > 0)) cw_voice_set_volume(v, 0);
  if (p->kind == P_3D && p->occlusion > 0) cw_voice_set_occlusion(v, p->occlusion);
  p->held = d.paused;
  int i = p->nvoices++;
  p->voices[i] = v;
  p->played[i] = (double)d.offset;
  /* a sound that loops by itself (its own loop points) never ends on its own */
  p->length[i] = cw_sound_get_loop(s, NULL, NULL) ? -2 : cw_sound_length_frames(s);
  p->rate[i] = rate;
  return v;
}

void cw_audio_player_set_sound(cw_node n, cw_sound sound) {
  player *p = player_of(n);
  if (!p) return;
  if (p->nvoices) cw_audio_player_stop(n);
  p->sound = sound;
}

void cw_audio_player_stop(cw_node n) {
  player *p = player_of(n);
  if (!p) return;
  for (int i = 0; i < p->nvoices; i++) cw_voice_stop(p->voices[i], 0);
  p->nvoices = 0;
  p->want_autoplay = false;
  if (p->music_on) cw_music_stop(p->music, 0);
  p->music_on = false;
}
bool cw_audio_player_is_playing(cw_node n) {
  player *p = player_of(n);
  return p && (p->nvoices > 0 || (p->music_on && cw_music_is_playing(p->music)));
}
cw_music cw_audio_player_music(cw_node n) {
  player *p = player_of(n);
  return p ? p->music : (cw_music){ 0 };
}
double cw_audio_player_position(cw_node n) {
  player *p = player_of(n);
  if (!p || !p->nvoices) return 0;
  int i = p->nvoices - 1;
  return p->rate[i] > 0 ? p->played[i] / p->rate[i] : 0;
}
cw_voice cw_audio_player_voice(cw_node n) {
  player *p = player_of(n);
  if (p && p->music_on) return cw_music_voice(p->music);
  return p && p->nvoices ? p->voices[p->nvoices - 1] : (cw_voice){ 0 };
}

/* ── the tick ───────────────────────────────────────────────────────── */

static void set_listener(int index, const t3_mat4 *m, t3_vec3 vel) {
  const float *e = m->e;
  cw_audio_set_listener_at(index, (t3_vec3){ e[12], e[13], e[14] }, vel, (t3_vec3){ -e[8], -e[9], -e[10] }, (t3_vec3){ e[4], e[5], e[6] });
}
static t3_vec3 velocity(t3_vec3 now, t3_vec3 prev, bool have, double dt) {
  if (!have || !(dt > 0)) return (t3_vec3){ 0, 0, 0 };
  float k = (float)(1.0 / dt);
  return (t3_vec3){ (now.x - prev.x) * k, (now.y - prev.y) * k, (now.z - prev.z) * k };
}

static void listeners_tick(cw_tree *t, tree_state *s, double dt) {
  if (!s->auto_listener || cw_audio_sample_rate() <= 0) return;
  int count = 0;
  for (int i = 0; i < s->listeners.count && count < CW_AUDIO_MAX_LISTENERS; i++) {
    cw_node n = s->listeners.data[i];
    listener *l = cw_node_data_t(n, &listener_type);
    const t3_mat4 *m = l && l->current ? cw_node3d_global_transform(n) : NULL;
    if (!m) continue;
    t3_vec3 pos = { m->e[12], m->e[13], m->e[14] };
    set_listener(count++, m, velocity(pos, l->prev_pos, l->have_prev, dt));
    l->prev_pos = pos;
    l->have_prev = true;
  }
  if (!count) {
    /* no listener node: the current camera hears, when the tree has 3D players */
    bool any3d = false;
    for (int i = 0; i < s->players.count && !any3d; i++) any3d = cw_node_data_t(s->players.data[i], &player3d_type) != NULL;
    cw_node cam = any3d ? cw_render3d_camera(t) : (cw_node){ 0 };
    const t3_mat4 *m = cw_node_valid(cam) ? cw_node3d_global_transform(cam) : NULL;
    if (!m) return;
    t3_vec3 pos = { m->e[12], m->e[13], m->e[14] };
    set_listener(0, m, velocity(pos, s->cam_prev_pos, s->cam_prev && s->cam_id == cam.id, dt));
    s->cam_prev_pos = pos;
    s->cam_prev = true;
    s->cam_id = cam.id;
    count = 1;
  }
  cw_audio_set_listener_count(count);
}

static void player_tick(cw_node n, player *p, double dt) {
  if (p->want_autoplay) {
    p->want_autoplay = false;
    cw_audio_player_play(n, 0);
  }
  if (p->music_on) { /* an interactive stream: held with the tree, placed in 3D; finished once when it stops by itself */
    bool hold = effective_hold(n, p);
    if (hold != p->held) {
      p->held = hold;
      cw_music_set_paused(p->music, hold);
    }
    if (!hold && p->kind == P_3D) {
      const t3_mat4 *m = cw_node3d_global_transform(n);
      t3_vec3 pos = { m->e[12], m->e[13], m->e[14] };
      cw_voice_3d sp = space_of(n, p, velocity(pos, p->prev_pos, p->have_prev, dt));
      p->prev_pos = pos;
      p->have_prev = true;
      cw_music_set_3d(p->music, &sp);
    }
    if (!hold && !cw_music_is_playing(p->music)) {
      p->music_on = false;
      cw_emit(n, s_finished, NULL, 0);
    }
    return;
  }
  if (!p->nvoices) {
    p->have_prev = false;
    return;
  }
  bool hold = effective_hold(n, p);
  if (hold != p->held) {
    p->held = hold;
    for (int i = 0; i < p->nvoices; i++) cw_voice_set_paused(p->voices[i], hold);
  }
  if (hold) return;
  if (p->kind == P_3D) {
    const t3_mat4 *m = cw_node3d_global_transform(n);
    t3_vec3 pos = { m->e[12], m->e[13], m->e[14] };
    cw_voice_3d sp = space_of(n, p, velocity(pos, p->prev_pos, p->have_prev, dt));
    p->prev_pos = pos;
    p->have_prev = true;
    for (int i = 0; i < p->nvoices; i++) cw_voice_set_3d(p->voices[i], &sp);
  } else if (p->kind == P_2D) {
    float g = p->gain2d, pan = p->pan2d;
    place2d(n, p);
    if (g != p->gain2d || pan != p->pan2d)
      for (int i = 0; i < p->nvoices; i++) {
        apply_gain(p, p->voices[i]);
        cw_voice_set_pan(p->voices[i], p->pan2d);
      }
  }
  /* count toward the end; a voice of unknown length ends when the mixer says so */
  float pitch = p->pitch_scale > 0 ? p->pitch_scale : 1;
  int ended = 0;
  for (int i = 0; i < p->nvoices;) {
    p->played[i] += (double)pitch * p->rate[i] * dt;
    bool done = false;
    if (!p->loop && p->length[i] != -2) done = p->length[i] >= 0 ? p->played[i] >= (double)p->length[i] : !cw_voice_is_playing(p->voices[i]);
    if (done) {
      drop_voice(p, i);
      ended++;
    } else i++;
  }
  for (int i = 0; i < ended && cw_node_valid(n); i++) cw_emit(n, s_finished, NULL, 0);
}

static void tick_hook(cw_tree *t, double dt, void *user) {
  (void)user;
  tree_state *s = cw_tree_slot(t, "audio_nodes");
  if (!s) return;
  if (!cw_tree_paused(t)) listeners_tick(t, s, dt);
  /* a handler of `finished` may free players: walk a copy */
  int n = s->players.count;
  if (!n) return;
  cw_node *copy = cw_frame_alloc(sizeof(cw_node) * (size_t)n);
  memcpy(copy, s->players.data, sizeof(cw_node) * (size_t)n);
  for (int i = 0; i < n; i++) {
    player *p = cw_node_valid(copy[i]) ? player_of(copy[i]) : NULL;
    if (p && p->in_tree) player_tick(copy[i], p, dt);
  }
}

static void frame_hook(cw_tree *t, double dt, void *user) {
  (void)dt, (void)user;
  tree_state *s = cw_tree_slot(t, "audio_nodes");
  if (s && s->own_audio && cw_audio_sample_rate() > 0) cw_audio_update();
}

/* ── node callbacks ─────────────────────────────────────────────────── */

static void init_common(player *p, int kind) {
  p->kind = kind;
  p->stream = cw_intern("");
  p->clip = cw_intern("");
  p->bus = cw_intern("Master");
  p->pitch_scale = 1;
  p->max_polyphony = 1;
  p->max_distance2d = 2000;
  p->attenuation = 1;
  p->panning_strength = 1;
  p->attenuation_model = CW_ATTEN_INVERSE;
  p->unit_size = 1;
  p->rolloff = 1;
  p->cone_inner_angle = p->cone_outer_angle = 360;
  p->reverb_send = 1;
  p->gain2d = 1;
}
static void init_plain(cw_node n, void *d) { (void)n; init_common(d, P_PLAIN); }
static void init_2d(cw_node n, void *d) { (void)n; init_common(d, P_2D); }
static void init_3d(cw_node n, void *d) { (void)n; init_common(d, P_3D); }

static void p_enter(cw_node n, void *d) {
  player *p = d;
  p->in_tree = true;
  p->have_prev = false;
  tree_state *s = state_of(cw_node_tree(n), true);
  if (s) cw_array_push(&s->players, n);
}
static void p_ready(cw_node n, void *d) {
  (void)n;
  player *p = d;
  if (p->autoplay) p->want_autoplay = true; /* on the first tick: the game's init has run, the mixer is its own */
}
static void p_exit(cw_node n, void *d) {
  player *p = d;
  cw_audio_player_stop(n);
  p->in_tree = false;
  tree_state *s = cw_tree_slot(cw_node_tree(n), "audio_nodes");
  if (s) list_remove(&s->players, n);
}
static void p_destroy(cw_node n, void *d) {
  (void)n;
  player *p = d;
  for (int i = 0; i < p->nvoices; i++) cw_voice_stop(p->voices[i], 0);
  p->nvoices = 0;
  if (!CW_HANDLE_IS_NULL(p->music)) cw_music_free(p->music);
  p->music = (cw_music){ 0 };
  p->music_on = false;
}
static void p_changed(cw_node n, void *d, const cw_prop *prop) {
  player *p = d;
  const char *nm = prop->name;
  if (!strcmp(nm, "clip")) {
    if (p->music_on && p->clip && *p->clip) cw_music_play(p->music, p->clip); /* the transition table decides when */
  } else if (!strcmp(nm, "volume_db")) {
    for (int i = 0; i < p->nvoices; i++) apply_gain(p, p->voices[i]);
    if (p->music_on) cw_music_set_volume(p->music, music_desc(p).volume);
  } else if (!strcmp(nm, "pitch_scale")) {
    for (int i = 0; i < p->nvoices; i++) cw_voice_set_pitch(p->voices[i], p->pitch_scale > 0 ? p->pitch_scale : 1);
  } else if (!strcmp(nm, "bus")) {
    int bus = cw_bus_find(p->bus);
    for (int i = 0; i < p->nvoices; i++) cw_voice_set_bus(p->voices[i], bus > 0 ? bus : 0);
  } else if (!strcmp(nm, "stream_paused")) {
    bool hold = effective_hold(n, p);
    if (hold != p->held) {
      p->held = hold;
      for (int i = 0; i < p->nvoices; i++) cw_voice_set_paused(p->voices[i], hold);
    }
  } else if (!strcmp(nm, "occlusion")) {
    for (int i = 0; i < p->nvoices; i++) cw_voice_set_occlusion(p->voices[i], p->occlusion);
  } else if (!strcmp(nm, "stream")) {
    if (p->nvoices || p->music_on) cw_audio_player_stop(n); /* another sound: what was playing stops */
    if (!CW_HANDLE_IS_NULL(p->music)) cw_music_free(p->music);
    p->music = (cw_music){ 0 };
  }
}
static void playing_get(cw_node n, void *d, cw_value *o) {
  *o = cw_val_bool(cw_audio_player_is_playing(n));
  (void)d;
}
static void current_clip_get(cw_node n, void *d, cw_value *o) {
  (void)n;
  const player *p = d;
  const char *c = p->music_on ? cw_music_current(p->music) : NULL;
  *o = cw_val_string(c ? c : "");
}
static void playing_set(cw_node n, void *d, const cw_value *in) {
  player *p = d;
  if (in->b && !p->nvoices) cw_audio_player_play(n, 0);
  else if (!in->b) cw_audio_player_stop(n);
}
static bool m_play(cw_node self, const cw_value *args, int argc, cw_value *ret, void *user) {
  (void)ret, (void)user;
  double from = 0;
  if (argc >= 1) {
    if (args[0].type == CW_T_FLOAT) from = args[0].f;
    else if (args[0].type == CW_T_INT) from = (double)args[0].i;
    else return false;
  }
  cw_audio_player_play(self, from);
  return true;
}
static bool m_stop(cw_node self, const cw_value *args, int argc, cw_value *ret, void *user) {
  (void)args, (void)argc, (void)ret, (void)user;
  cw_audio_player_stop(self);
  return true;
}

#define PF(field, ty, docs) CW_PROP_FIELD(player, field, ty, CW_PROP_DEFAULT, docs)
#define PR(field, lo, hi, docs) CW_PROP_FIELD_RANGE(player, field, CW_T_FLOAT, lo, hi, docs)
#define COMMON_PROPS \
  PF(stream, CW_T_STRING, "The sound: an asset path, or a .cwaudio stream resource (a randomizer or an interactive music, cartwheel/audio.h). Loaded on first play, shared by every player naming it."), \
  PF(clip, CW_T_STRING, "An interactive stream: the clip to play; set while it plays to switch by its transitions (empty = its initial clip)."), \
  CW_PROP_ACCESSOR("current_clip", CW_T_STRING, CW_PROP_EDITOR, current_clip_get, NULL, "An interactive stream: the clip sounding now (not saved)."), \
  PR(volume_db, -80, 24, "Gain in decibels: 0 as recorded, -6 half the amplitude, -80 and below silent."), \
  PR(pitch_scale, 0.01, 4, "Playback rate (1 = as recorded)."), \
  PF(autoplay, CW_T_BOOL, "Start on the first fixed tick after the node is ready."), \
  PF(loop, CW_T_BOOL, "Repeat until stopped."), \
  PF(bus, CW_T_STRING, "The mixer bus by name (\"Master\", or a name given to cw_bus_create)."), \
  { .name = "max_polyphony", .type = CW_T_INT, .flags = CW_PROP_DEFAULT, .offset = (int)offsetof(player, max_polyphony), \
    .size = (int)sizeof(int), .min = 1, .max = MAX_POLY, .doc = "Copies that may sound at once; one more stops the oldest." }, \
  PF(stream_paused, CW_T_BOOL, "Hold every voice where it is."), \
  CW_PROP_ACCESSOR("playing", CW_T_BOOL, CW_PROP_EDITOR, playing_get, playing_set, \
                   "true while a voice sounds or is held; set true to play, false to stop (not saved).")

static const cw_prop player_props[] = { COMMON_PROPS, CW_PROP_END };
static const cw_prop player2d_props[] = {
  COMMON_PROPS,
  { .name = "max_distance", .type = CW_T_FLOAT, .flags = CW_PROP_DEFAULT, .offset = (int)offsetof(player, max_distance2d),
    .size = (int)sizeof(float), .min = 0, .max = 1e9, .doc = "Pixels from the middle of the view at which the sound is silent (0 = never fades)." },
  PR(attenuation, 0, 16, "The fade's exponent: gain = (1 - distance / max_distance) ^ attenuation."),
  PR(panning_strength, 0, 1, "How far a sound at the edge of the view is panned (0 none, 1 hard)."),
  CW_PROP_END,
};
static const cw_prop player3d_props[] = {
  COMMON_PROPS,
  { .name = "attenuation_model", .type = CW_T_ENUM, .flags = CW_PROP_DEFAULT, .offset = (int)offsetof(player, attenuation_model),
    .size = (int)sizeof(int), .enum_names = atten_names, .doc = "How the sound fades with distance (cw_atten_model)." },
  PR(unit_size, 0, 1e6, "Distance at which the sound is at full volume."),
  PR(max_distance, 0, 1e9, "Beyond this it gets no quieter (0 = 1000)."),
  PR(rolloff, 0, 100, "How fast it fades with distance."),
  PR(cone_inner_angle, 0, 360, "Degrees (full width) about the node's -Z at full volume; 360 = every direction."),
  PR(cone_outer_angle, 0, 360, "Degrees (full width) beyond which the gain is cone_outer_gain."),
  PR(cone_outer_gain, 0, 1, "Gain outside the outer cone."),
  PR(doppler, 0, 10, "Doppler factor: 0 none, 1 physical."),
  PR(air_absorption, 0, 10, "Distance low-pass (cw_voice_3d.air_absorption); 0 = none."),
  PR(reverb_send, 0, 10, "Level into the reverb zones (0 = none)."),
  PR(occlusion, 0, 1, "0 clear .. 1 blocked: a low-pass the game sets."),
  PF(panning_only, CW_T_BOOL, "Always pan left/right, never the head-related filter."),
  CW_PROP_END,
};
static const char *const player_signals[] = { "finished", NULL };

static cw_node_type player_type = { .name = "AudioStreamPlayer", .parent = "Node", .data_size = sizeof(player), .props = player_props,
                                    .signals = player_signals, .doc = "Plays a sound with no place: music, UI (cartwheel/audio_nodes.h).",
                                    .init = init_plain, .destroy = p_destroy, .enter_tree = p_enter, .ready = p_ready, .exit_tree = p_exit,
                                    .prop_changed = p_changed };
static cw_node_type player2d_type = { .name = "AudioStreamPlayer2D", .parent = "Node2D", .data_size = sizeof(player), .props = player2d_props,
                                      .signals = player_signals,
                                      .doc = "Plays a sound panned and faded by where the node is on the 2D view (cartwheel/audio_nodes.h).",
                                      .init = init_2d, .destroy = p_destroy, .enter_tree = p_enter, .ready = p_ready, .exit_tree = p_exit,
                                      .prop_changed = p_changed };
static cw_node_type player3d_type = { .name = "AudioStreamPlayer3D", .parent = "Node3D", .data_size = sizeof(player), .props = player3d_props,
                                      .signals = player_signals,
                                      .doc = "Plays a sound at the node: distance, cone, doppler, reverb zones (cartwheel/audio_nodes.h).",
                                      .init = init_3d, .destroy = p_destroy, .enter_tree = p_enter, .ready = p_ready, .exit_tree = p_exit,
                                      .prop_changed = p_changed };

static void l_init(cw_node n, void *d) {
  (void)n;
  ((listener *)d)->current = true;
}
static void l_enter(cw_node n, void *d) {
  ((listener *)d)->have_prev = false;
  tree_state *s = state_of(cw_node_tree(n), true);
  if (s) cw_array_push(&s->listeners, n);
}
static void l_exit(cw_node n, void *d) {
  (void)d;
  tree_state *s = cw_tree_slot(cw_node_tree(n), "audio_nodes");
  if (s) list_remove(&s->listeners, n);
}
static const cw_prop listener_props[] = {
  CW_PROP_FIELD(listener, current, CW_T_BOOL, CW_PROP_DEFAULT, "This node is a listener (several: split screen, each voice heard by the nearest)."),
  CW_PROP_END,
};
static cw_node_type listener_type = { .name = "AudioListener3D", .parent = "Node3D", .data_size = sizeof(listener), .props = listener_props,
                                      .doc = "The ears of 3D sound; without one the current Camera3D listens (cartwheel/audio_nodes.h).",
                                      .init = l_init, .enter_tree = l_enter, .exit_tree = l_exit };

void cw__register_audio_player_types(void);
void cw__register_audio_player_types(void) {
  s_finished = cw_intern("finished");
  cw_node_type_register(&player_type);
  cw_node_type_register(&player2d_type);
  cw_node_type_register(&player3d_type);
  cw_node_type_register(&listener_type);
  static const char *const names[] = { "AudioStreamPlayer", "AudioStreamPlayer2D", "AudioStreamPlayer3D" };
  for (int i = 0; i < 3; i++) {
    cw_node_type_add_method(names[i], "play", "...", m_play, NULL, "Play the stream; an optional float starts that many seconds in.");
    cw_node_type_add_method(names[i], "stop", NULL, m_stop, NULL, "Stop every voice at once (no finished).");
  }
}

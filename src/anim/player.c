/* AnimationPlayer: a library of animations, one current playback, cross-fades,
 * a queue; values go through the shared mixer. */
#include <stdio.h>
#include <stdlib.h>

#include "anim_internal.h"

#define MAX_FADES 4

typedef struct playback {
  cw_anim anim;
  const char *name;
  anim_cursor cur;
  float speed;
  float fade_len, fade_t; /* current: fade-in; fading list: fade-out */
} playback;

typedef struct { const char *name; cw_anim anim; } lib_entry;
typedef struct { const char *from, *to; float time; } blend_pair;

typedef struct player {
  /* properties */
  const char *root_node;
  const char *autoplay;
  float speed_scale;
  float default_blend;
  int32_t process;
  const char *root_motion_track;
  const char *root_motion_target;
  int32_t root_motion_lock;
  /* state */
  CW_ARRAY(lib_entry) lib;
  bool has_cur, playing;
  playback cur;
  playback fading[MAX_FADES];
  int nfading;
  CW_ARRAY(const char *) queue;
  CW_ARRAY(blend_pair) blends;
  cw__amix *mix;
  anim_fired_list fired;
} player;

static const cw_node_type *player_t;

static player *P(cw_node n, const char *fn) {
  player *p = cw_node_data_t(n, player_t);
  if (!p && fn) cw_warn("%s: '%s' is not an AnimationPlayer", fn, cw_node_valid(n) ? cw_node_name(n) : "(invalid node)");
  return p;
}

static void pb_set(playback *pb, cw_anim a, const char *name) {
  if (pb->anim.id != a.id) {
    cw_anim_retain(a);
    cw_anim_release(pb->anim);
  }
  pb->anim = a;
  pb->name = name;
}
static void pb_drop(playback *pb) {
  cw_anim_release(pb->anim);
  memset(pb, 0, sizeof *pb);
}

/* ── library ────────────────────────────────────────────────────────── */

void cw_anim_player_add(cw_node n, const char *name, cw_anim a) {
  player *p = P(n, "cw_anim_player_add");
  if (!p || !cw_anim_valid(a)) return;
  const char *in = cw_intern(name ? name : cw_anim_name(a));
  cw_anim_retain(a);
  for (int i = 0; i < p->lib.count; i++)
    if (p->lib.data[i].name == in) {
      cw_anim_release(p->lib.data[i].anim);
      p->lib.data[i].anim = a;
      return;
    }
  cw_array_push(&p->lib, ((lib_entry){ in, a }));
}

void cw_anim_player_remove(cw_node n, const char *name) {
  player *p = P(n, "cw_anim_player_remove");
  if (!p) return;
  const char *in = cw_intern(name);
  for (int i = 0; i < p->lib.count; i++)
    if (p->lib.data[i].name == in) {
      cw_anim_release(p->lib.data[i].anim);
      memmove(p->lib.data + i, p->lib.data + i + 1, sizeof *p->lib.data * (size_t)(p->lib.count - i - 1));
      p->lib.count--;
      return;
    }
}

cw_anim cw__player_lookup(cw_node n, const char *name) {
  player *p = P(n, NULL);
  if (!p || !name) return (cw_anim){ 0 };
  const char *in = cw_intern(name);
  for (int i = 0; i < p->lib.count; i++)
    if (p->lib.data[i].name == in) return p->lib.data[i].anim;
  return (cw_anim){ 0 };
}
cw_anim cw_anim_player_get(cw_node n, const char *name) { return cw__player_lookup(n, name); }
int cw_anim_player_count(cw_node n) { player *p = P(n, "cw_anim_player_count"); return p ? p->lib.count : 0; }
const char *cw_anim_player_name_at(cw_node n, int i) {
  player *p = P(n, "cw_anim_player_name_at");
  return p && i >= 0 && i < p->lib.count ? p->lib.data[i].name : NULL;
}

cw_node cw__player_root(cw_node n) {
  player *p = P(n, NULL);
  if (!p) return (cw_node){ 0 };
  const char *r = p->root_node && *p->root_node ? p->root_node : "..";
  return strcmp(r, ".") ? cw_node_get(n, r) : n;
}

/* ── playback ───────────────────────────────────────────────────────── */

static float pair_blend(player *p, const char *from, const char *to) {
  const char *star = cw_intern("*");
  for (int i = p->blends.count - 1; i >= 0; i--) {
    blend_pair *b = &p->blends.data[i];
    if ((b->from == from || b->from == star) && (b->to == to || b->to == star)) return b->time;
  }
  return p->default_blend;
}

void cw_anim_player_set_blend(cw_node n, const char *from, const char *to, float seconds) {
  player *p = P(n, "cw_anim_player_set_blend");
  if (!p) return;
  cw_array_push(&p->blends, ((blend_pair){ cw_intern(from), cw_intern(to), seconds }));
}

static void start(cw_node n, player *p, const char *name, cw_anim a, float blend, float speed, bool from_end) {
  const char *old = p->has_cur ? p->cur.name : NULL;
  if (blend < 0) blend = old ? pair_blend(p, old, name) : 0;
  if (p->has_cur && p->playing && blend > 0) {
    if (p->nfading == MAX_FADES) {
      pb_drop(&p->fading[0]);
      memmove(p->fading, p->fading + 1, sizeof(playback) * (MAX_FADES - 1));
      memset(&p->fading[MAX_FADES - 1], 0, sizeof(playback));
      p->nfading--;
    }
    playback *f = &p->fading[p->nfading++];
    *f = p->cur;
    cw_anim_retain(f->anim);
    f->fade_len = blend;
    f->fade_t = 0;
  } else {
    for (int i = 0; i < p->nfading; i++) pb_drop(&p->fading[i]);
    p->nfading = 0;
  }
  pb_set(&p->cur, a, name);
  memset(&p->cur.cur, 0, sizeof p->cur.cur);
  p->cur.cur.time = from_end ? cw_anim_length(a) : 0;
  p->cur.cur.dir = 1;
  p->cur.cur.fire_start = true;
  p->cur.speed = speed;
  p->cur.fade_len = p->nfading ? blend : 0;
  p->cur.fade_t = 0;
  p->has_cur = true;
  p->playing = true;
  if (old != name) {
    cw_value args[2] = { cw_val_string(old ? old : ""), cw_val_string(name) };
    if (old) cw_emit(n, "animation_changed", args, 2);
  }
  cw_value s = cw_val_string(name);
  cw_emit(n, "animation_started", &s, 1);
}

void cw_anim_player_play_ex(cw_node n, const char *name, float blend, float speed, bool from_end) {
  player *p = P(n, "cw_anim_player_play");
  if (!p) return;
  if (!name || !*name) {
    if (!p->has_cur) { cw_warn("cw_anim_player_play('%s', NULL): nothing to resume (name an animation)", cw_node_name(n)); return; }
    p->playing = true;
    return;
  }
  const char *in = cw_intern(name);
  cw_anim a = cw__player_lookup(n, in);
  if (!cw_anim_valid(a)) {
    cw_warn("cw_anim_player_play: '%s' has no animation '%s' (%d in its library; add with cw_anim_player_add)",
            cw_node_name(n), name, p->lib.count);
    return;
  }
  if (p->has_cur && p->cur.name == in && p->cur.anim.id == a.id && !p->cur.cur.finished && !from_end) {
    p->cur.speed = speed;
    p->playing = true;
    return;
  }
  start(n, p, in, a, blend, speed, from_end);
}

void cw_anim_player_play(cw_node n, const char *name, float blend) { cw_anim_player_play_ex(n, name, blend, 1, false); }

void cw_anim_player_pause(cw_node n) {
  player *p = P(n, "cw_anim_player_pause");
  if (p) p->playing = false;
}

void cw_anim_player_stop(cw_node n, bool keep_state) {
  player *p = P(n, "cw_anim_player_stop");
  if (!p) return;
  p->playing = false;
  for (int i = 0; i < p->nfading; i++) pb_drop(&p->fading[i]);
  p->nfading = 0;
  cw_array_clear(&p->queue);
  if (!keep_state && p->has_cur) {
    p->cur.cur.time = 0;
    p->cur.cur.dir = 1;
    p->cur.cur.finished = false;
    p->cur.cur.fire_start = true;
  }
}

static void configure_mixer(cw_node n, player *p) {
  cw__amix_set_root(p->mix, cw__player_root(n));
  p->mix->rm_track = p->root_motion_track && *p->root_motion_track ? p->root_motion_track : NULL;
  p->mix->rm_lock = p->root_motion_lock;
  p->mix->rm_target = p->root_motion_target && *p->root_motion_target ? cw_node_get(n, p->root_motion_target) : (cw_node){ 0 };
}

void cw_anim_player_seek(cw_node n, float time, bool update) {
  player *p = P(n, "cw_anim_player_seek");
  if (!p || !p->has_cur) return;
  float len = cw_anim_length(p->cur.anim);
  p->cur.cur.time = time < 0 ? 0 : time > len ? len : time;
  p->cur.cur.fire_start = false;
  p->cur.cur.finished = false;
  if (update) {
    configure_mixer(n, p);
    cw__amix_sample(p->mix, p->cur.anim, (float)p->cur.cur.time, 1, NULL, false, NULL);
    cw__amix_apply(p->mix);
  }
}

void cw_anim_player_queue(cw_node n, const char *name) {
  player *p = P(n, "cw_anim_player_queue");
  if (!p) return;
  if (!p->has_cur || !p->playing) { cw_anim_player_play(n, name, -1); return; }
  cw_array_push(&p->queue, cw_intern(name));
}
void cw_anim_player_clear_queue(cw_node n) {
  player *p = P(n, "cw_anim_player_clear_queue");
  if (p) cw_array_clear(&p->queue);
}

const char *cw_anim_player_current(cw_node n) { player *p = P(n, NULL); return p && p->has_cur ? p->cur.name : NULL; }
bool cw_anim_player_is_playing(cw_node n) { player *p = P(n, NULL); return p && p->playing; }
double cw_anim_player_position(cw_node n) { player *p = P(n, NULL); return p && p->has_cur ? p->cur.cur.time : 0; }
uint32_t cw_anim_player_hash(cw_node n) { player *p = P(n, NULL); return p ? p->mix->hash : 0; }
t3_vec3 cw_anim_player_root_motion(cw_node n) { player *p = P(n, NULL); return p ? p->mix->rm_last : (t3_vec3){ 0, 0, 0 }; }

/* ── the tick ───────────────────────────────────────────────────────── */

static void dispatch(cw_node n, player *p, cw_anim a, const char *name, int count) {
  cw_anim_retain(a);
  anim_rec *r = cw__anim(a);
  cw_node root = cw__player_root(n);
  for (int i = 0; i < count && i < p->fired.count && r; i++) {
    anim_fired f = p->fired.data[i];
    cw_value s = cw_val_string(name);
    if (f.kind == 0) {
      anim_track *t = &r->tracks.data[f.track];
      anim_mkey *k = &t->mkeys.data[f.key];
      if (t->kind == CW_TRACK_METHOD) cw__fire_method_key(root, t, k);
      else {
        cw_value args[2] = { cw_val_string(k->name), s };
        cw_emit(n, "animation_event", args, 2);
      }
    } else if (f.kind == 1) {
      cw_emit(n, "animation_looped", &s, 1);
    } else if (f.kind == 2) {
      if (p->has_cur && p->cur.anim.id == a.id) p->playing = false;
      cw_emit(n, "animation_finished", &s, 1);
      if (p->queue.count && cw_node_valid(n)) {
        const char *next = p->queue.data[0];
        memmove(p->queue.data, p->queue.data + 1, sizeof(char *) * (size_t)(p->queue.count - 1));
        p->queue.count--;
        start(n, p, next, cw__player_lookup(n, next), -1, 1, false);
        if (!cw_anim_valid(p->cur.anim)) p->has_cur = p->playing = false;
      }
    }
    r = cw__anim(a);
    p = cw_node_data_t(n, player_t); /* a callback may have freed the player */
    if (!p) break;
  }
  cw_anim_release(a);
}

static void advance(cw_node n, player *p, double dt) {
  if (!p->has_cur || !p->playing || dt == 0) return;
  configure_mixer(n, p);
  cw_array_clear(&p->fired);
  anim_rec *r = cw__anim(p->cur.anim);
  if (!r) { p->has_cur = p->playing = false; return; }
  cw__cursor_advance(r, &p->cur.cur, dt * p->speed_scale * p->cur.speed, &p->fired);
  for (int i = 0; i < p->nfading;) {
    playback *f = &p->fading[i];
    anim_rec *fr = cw__anim(f->anim);
    f->fade_t += (float)dt;
    if (!fr || f->fade_t >= f->fade_len) {
      pb_drop(f);
      memmove(p->fading + i, p->fading + i + 1, sizeof(playback) * (size_t)(p->nfading - i - 1));
      memset(&p->fading[--p->nfading], 0, sizeof(playback));
      continue;
    }
    cw__cursor_advance(fr, &f->cur, dt * p->speed_scale * f->speed, NULL);
    i++;
  }
  p->cur.fade_t += (float)dt;
  for (int i = 0; i < p->nfading; i++) {
    playback *f = &p->fading[i];
    cw__amix_sample(p->mix, f->anim, (float)f->cur.time, 1 - f->fade_t / f->fade_len, NULL, false, &f->cur);
  }
  float w = p->cur.fade_len > 0 && p->cur.fade_t < p->cur.fade_len ? p->cur.fade_t / p->cur.fade_len : 1;
  cw__amix_sample(p->mix, p->cur.anim, (float)p->cur.cur.time, w, NULL, false, &p->cur.cur);
  cw__amix_apply(p->mix);
  dispatch(n, p, p->cur.anim, p->cur.name, p->fired.count);
}

void cw_anim_player_advance(cw_node n, double dt) {
  player *p = P(n, "cw_anim_player_advance");
  if (p) advance(n, p, dt);
}

/* ── the node type ──────────────────────────────────────────────────── */

static void p_init(cw_node n, void *d) {
  player *p = d;
  p->root_node = cw_intern("..");
  p->speed_scale = 1;
  p->root_motion_lock = 7;
  p->mix = cw_alloc(sizeof *p->mix, CW_MEM_ANIM);
  cw__amix_init(p->mix, n);
}

static void p_destroy(cw_node n, void *d) {
  (void)n;
  player *p = d;
  for (int i = 0; i < p->lib.count; i++) cw_anim_release(p->lib.data[i].anim);
  cw_array_free(&p->lib);
  pb_drop(&p->cur);
  for (int i = 0; i < p->nfading; i++) pb_drop(&p->fading[i]);
  cw_array_free(&p->queue);
  cw_array_free(&p->blends);
  cw_array_free(&p->fired);
  cw__amix_free(p->mix);
  cw_free(p->mix);
}

static void p_ready(cw_node n, void *d) {
  player *p = d;
  if (p->autoplay && *p->autoplay && !p->has_cur) cw_anim_player_play(n, p->autoplay, 0);
}

static void p_fixed(cw_node n, void *d, double dt) {
  player *p = d;
  if (p->process == CW_ANIM_PROCESS_FIXED) advance(n, p, dt);
}
static void p_update(cw_node n, void *d, double dt) {
  player *p = d;
  if (p->process == CW_ANIM_PROCESS_UPDATE) advance(n, p, dt);
}

static void cur_get(cw_node n, void *d, cw_value *o) {
  (void)n;
  player *p = d;
  *o = cw_val_string(p->has_cur ? p->cur.name : "");
}
static void cur_set(cw_node n, void *d, const cw_value *v) {
  (void)d;
  if (v->s && *v->s) cw_anim_player_play(n, v->s, -1);
  else cw_anim_player_stop(n, true);
}
static void playing_get(cw_node n, void *d, cw_value *o) { (void)n; *o = cw_val_bool(((player *)d)->playing); }
static void pos_get(cw_node n, void *d, cw_value *o) {
  (void)n;
  player *p = d;
  *o = cw_val_float(p->has_cur ? p->cur.cur.time : 0);
}

const char *const cw__anim_process_names[] = { "FIXED", "UPDATE", "MANUAL", NULL };

static const cw_prop player_props[] = {
  CW_PROP_FIELD(player, root_node, CW_T_STRING, CW_PROP_STORED | CW_PROP_EDITOR, "Track paths are relative to this node."),
  CW_PROP_FIELD(player, autoplay, CW_T_STRING, CW_PROP_STORED | CW_PROP_EDITOR, "Played when the player is ready."),
  CW_PROP_FIELD(player, speed_scale, CW_T_FLOAT, CW_PROP_DEFAULT, "Multiplies every playback speed."),
  CW_PROP_FIELD(player, default_blend, CW_T_FLOAT, CW_PROP_DEFAULT, "Cross-fade seconds when play() passes blend < 0."),
  { .name = "process", .type = CW_T_ENUM, .flags = CW_PROP_STORED | CW_PROP_EDITOR, .offset = offsetof(player, process),
    .size = sizeof(int32_t), .enum_names = cw__anim_process_names, .doc = "Which phase advances it (MANUAL: cw_anim_player_advance)." },
  CW_PROP_ACCESSOR("current_animation", CW_T_STRING, CW_PROP_EDITOR | CW_PROP_ANIMATABLE, cur_get, cur_set,
                   "The playing animation; setting it plays that animation."),
  CW_PROP_ACCESSOR("playing", CW_T_BOOL, CW_PROP_EDITOR | CW_PROP_READONLY, playing_get, NULL, "Advancing now."),
  CW_PROP_ACCESSOR("position", CW_T_FLOAT, CW_PROP_EDITOR | CW_PROP_READONLY, pos_get, NULL, "Time in the current animation."),
  CW_PROP_FIELD(player, root_motion_track, CW_T_STRING, CW_PROP_STORED | CW_PROP_EDITOR, "Position track whose motion moves root_motion_target."),
  CW_PROP_FIELD(player, root_motion_target, CW_T_STRING, CW_PROP_STORED | CW_PROP_EDITOR, "Node3D (path from the player) moved by root motion."),
  CW_PROP_FIELD(player, root_motion_lock, CW_T_INT, CW_PROP_STORED | CW_PROP_EDITOR, "Axes given to the target: bit 0 x, 1 y, 2 z."),
  CW_PROP_END,
};

static const char *const player_signals[] = { "animation_started", "animation_finished", "animation_changed",
                                              "animation_looped", "animation_event", NULL };

static cw_node_type player_type = {
  .name = "AnimationPlayer",
  .parent = "Node",
  .data_size = sizeof(player),
  .props = player_props,
  .signals = player_signals,
  .doc = "Plays animations from its library on properties of the nodes under root_node.",
  .init = p_init,
  .destroy = p_destroy,
  .ready = p_ready,
  .fixed_update = p_fixed,
  .update = p_update,
};

void cw__register_player_type(void) {
  cw_node_type_register(&player_type);
  player_t = &player_type;
}

/* ── debug channel ──────────────────────────────────────────────────── */

#include "cartwheel/debug.h"

static void cmd_anim(int argc, const char **argv, cw_reply *r) {
  if (argc < 2) { cw_reply_error(r, "usage: anim <player path>"); return; }
  cw_node n = cw__anim_debug_node(argv[1], r, "AnimationPlayer");
  if (!cw_node_valid(n)) return;
  player *p = P(n, NULL);
  cw_reply_raw(r, "{\"current\":");
  cw_reply_str(r, p->has_cur ? p->cur.name : "");
  cw_reply_printf(r, ",\"playing\":%s,\"position\":%.9g,\"length\":%.9g,\"fading\":%d,\"queue\":%d,\"hash\":%u,\"library\":[",
                  p->playing ? "true" : "false", p->has_cur ? p->cur.cur.time : 0.0,
                  p->has_cur ? (double)cw_anim_length(p->cur.anim) : 0.0, p->nfading, p->queue.count, p->mix->hash);
  for (int i = 0; i < p->lib.count; i++) {
    if (i) cw_reply_raw(r, ",");
    cw_reply_str(r, p->lib.data[i].name);
  }
  cw_reply_raw(r, "]}");
}

static void cmd_play(int argc, const char **argv, cw_reply *r) {
  if (argc < 3) { cw_reply_error(r, "usage: anim_play <player path> <animation> [blend seconds] [speed]"); return; }
  cw_node n = cw__anim_debug_node(argv[1], r, "AnimationPlayer");
  if (!cw_node_valid(n)) return;
  if (!cw_anim_valid(cw__player_lookup(n, argv[2]))) { cw_reply_error(r, "no animation '%s' (try: anim %s)", argv[2], argv[1]); return; }
  cw_anim_player_play_ex(n, argv[2], argc > 3 ? strtof(argv[3], NULL) : -1, argc > 4 ? strtof(argv[4], NULL) : 1, false);
  cw_reply_raw(r, "{\"ok\":true}");
}

static void cmd_stop(int argc, const char **argv, cw_reply *r) {
  if (argc < 2) { cw_reply_error(r, "usage: anim_stop <player path> [keep 0|1]"); return; }
  cw_node n = cw__anim_debug_node(argv[1], r, "AnimationPlayer");
  if (!cw_node_valid(n)) return;
  cw_anim_player_stop(n, argc > 2 && atoi(argv[2]));
  cw_reply_raw(r, "{\"ok\":true}");
}

static void cmd_seek(int argc, const char **argv, cw_reply *r) {
  if (argc < 3) { cw_reply_error(r, "usage: anim_seek <player path> <seconds>"); return; }
  cw_node n = cw__anim_debug_node(argv[1], r, "AnimationPlayer");
  if (!cw_node_valid(n)) return;
  cw_anim_player_seek(n, strtof(argv[2], NULL), true);
  cw_reply_raw(r, "{\"ok\":true}");
}

static void cmd_text(int argc, const char **argv, cw_reply *r) {
  if (argc < 3) { cw_reply_error(r, "usage: anim_text <player path> <animation>: the animation as scene-file text"); return; }
  cw_node n = cw__anim_debug_node(argv[1], r, "AnimationPlayer");
  if (!cw_node_valid(n)) return;
  cw_anim a = cw__player_lookup(n, argv[2]);
  if (!cw_anim_valid(a)) { cw_reply_error(r, "no animation '%s'", argv[2]); return; }
  char *t = cw__anim_to_text_named(a, argv[2], ".");
  cw_reply_str(r, t);
  cw_free(t);
}

void cw__register_player_debug_commands(void) {
  cw_debug_command("anim", "anim <player path>: current animation, position, playing, library, pose hash", cmd_anim);
  cw_debug_command("anim_play", "anim_play <player path> <animation> [blend] [speed]: play (cross-fade)", cmd_play);
  cw_debug_command("anim_stop", "anim_stop <player path> [keep]: stop (keep=1 keeps the position)", cmd_stop);
  cw_debug_command("anim_seek", "anim_seek <player path> <seconds>: jump and write the values now", cmd_seek);
  cw_debug_command("anim_text", "anim_text <player path> <animation>: the animation as .cwscene text", cmd_text);
}

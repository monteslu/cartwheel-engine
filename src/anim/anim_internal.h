#ifndef CW_ANIM_INTERNAL_H
#define CW_ANIM_INTERNAL_H

#include "cartwheel/anim.h"

/* ── resources ─────────────────────────────────────────────────────── */

typedef struct anim_mkey {
  const char *name; /* method or event name (interned) */
  int argc;
  cw_value args[CW_ANIM_MAX_ARGS];
} anim_mkey;

typedef struct anim_track {
  cw_track_kind kind;
  const char *path;     /* as written (interned) */
  const char *node;     /* node path part (interned; "" = the root) */
  const char *prop;     /* property ("position", "position.y"), NULL for method/event */
  const char *bone;     /* bone name for bone tracks, else NULL */
  cw_interp interp;
  cw_rot_mode rot;
  bool enabled;
  CW_ARRAY(float) times;
  CW_ARRAY(cw_value) values;   /* value tracks */
  CW_ARRAY(cw_value) tangents; /* HERMITE: in, out per key */
  CW_ARRAY(anim_mkey) mkeys;   /* method / event tracks */
} anim_track;

typedef struct anim_rec {
  int refs;
  const char *name;
  float length;
  cw_loop_mode loop;
  bool stored;
  uint32_t version; /* bumped on any track change (binding caches) */
  CW_ARRAY(anim_track) tracks;
} anim_rec;

anim_rec *cw__anim(cw_anim a);
/* sample a value/bone track; want != NONE coerces the keys (int -> float...) */
bool cw__track_sample(const anim_track *t, float time, cw_type want, cw_value *out);
int cw__value_ncomp(cw_type t);
bool cw__value_interpolable(cw_type t);
/* dst := mix(dst, src, w) for a value of type t (quats slerp/nlerp) */
void cw__value_mix(cw_value *dst, const cw_value *src, float w, cw_rot_mode rot);
bool cw__value_coerce(cw_value *v, cw_type want);

/* ── key sweeps (method / event firing) ────────────────────────────── */

typedef struct anim_fired {
  int kind; /* 0 method/event key, 1 looped, 2 finished, 3 started, 4 changed */
  int track, key;
  const char *a, *b;
} anim_fired;
typedef CW_ARRAY(anim_fired) anim_fired_list;

/* A playback cursor: time, ping-pong direction, and whether keys at the
 * current time are still to fire. */
typedef struct anim_cursor {
  double time;
  int dir;           /* +1 / -1: ping-pong travel direction */
  bool fire_start;   /* keys at `time` fire on the next sweep */
  bool finished;     /* NONE: reached the end in the direction of motion */
  bool ended;        /* this advance finished, wrapped or turned (state machines) */
  /* root motion: the sweep as (from, to) segments in clip time */
  int seg_count;
  float seg[8][2];
} anim_cursor;

/* Advance by delta (seconds of clip time, signed). Fires into `out` (may be
 * NULL: no keys) and records segments for root motion. */
void cw__cursor_advance(anim_rec *a, anim_cursor *c, double delta, anim_fired_list *out);

/* ── the mixer (shared by AnimationPlayer and AnimationTree) ───────── */

typedef struct mix_binding {
  uint8_t kind;       /* 0 node property, 1 bone, 2 dead */
  cw_node node;
  const char *prop;   /* property name actually set */
  int comp;           /* -1 whole value, else component */
  cw_type type;       /* the bound value's type */
  cw_rot_mode rot;
  t3_object *obj;     /* bone */
  int bprop;          /* 0 position, 1 quaternion, 2 scale */
  cw_value base;      /* rest value (captured on bind) */
  cw_value acc;       /* accumulated this tick */
  float weight;
  bool touched;
  float add[4];       /* additive accumulation (quat: a rotation) */
  bool has_add;
  bool root_motion;
} mix_binding;

typedef struct mix_cache {
  uint64_t anim;
  uint32_t version;
  int *idx; /* per track: binding index or -1 */
  int n;
} mix_cache;

typedef struct cw__amix {
  cw_node owner;      /* player or tree node */
  cw_node root;       /* node track paths resolve from */
  CW_ARRAY(mix_binding) b;
  cw_map index;       /* key -> binding index + 1 */
  CW_ARRAY(mix_cache) caches;
  uint64_t struct_ver;
  uint32_t hash;
  /* root motion */
  const char *rm_track; /* interned path, NULL = none */
  cw_node rm_target;
  int rm_lock;
  t3_vec3 rm_acc;       /* weighted delta, track parent space */
  float rm_weight;
  t3_vec3 rm_last;      /* last tick's motion, target parent space */
  int rm_binding;
} cw__amix;

void cw__amix_init(cw__amix *m, cw_node owner);
void cw__amix_free(cw__amix *m);
void cw__amix_set_root(cw__amix *m, cw_node root);
/* Accumulate one animation at `time` with weight w. filter: NULL or a map of
 * interned names (bones / node paths) to allow. additive: add mode. cursor:
 * for root motion segments (may be NULL). */
void cw__amix_sample(cw__amix *m, cw_anim anim, float time, float w, const cw_map *filter, bool additive,
                      const anim_cursor *cursor);
/* Write every touched binding, fold into the hash, apply root motion. */
void cw__amix_apply(cw__amix *m);

/* shared root-motion property plumbing */
void cw__fire_method_key(cw_node root, const anim_track *t, const anim_mkey *k);

char *cw__anim_to_text_named(cw_anim a, const char *name, const char *player_path);
const char *cw__anim_loop_name(int i);
const char *cw__anim_interp_name(int i);
const char *cw__anim_kind_name(int i);
/* debug-channel helper: a node by path from the current tree's root */
cw_node cw__anim_debug_node(const char *path, void *reply, const char *type);
void cw__register_player_debug_commands(void);
void cw__register_tree_debug_commands(void);
void cw__register_anim_scene_section(void);
void cw__register_player_type(void);
void cw__register_tree_type(void);
/* the player's library and root for an AnimationTree */
cw_anim cw__player_lookup(cw_node player, const char *name);
cw_node cw__player_root(cw_node player);

#endif

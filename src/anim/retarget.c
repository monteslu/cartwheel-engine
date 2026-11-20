/* Retargeting core: humanoid bone maps (from names, from text) and the
 * rest-relative transfer of one pose onto another skeleton, with foot and
 * hand IK. Poses only: no scene tree, no clips (retarget_clip.c), so cwpack
 * links it too. Deterministic: + - * / sqrt and cartwheel/math.h. */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cartwheel/retarget.h"

/* ── helpers ────────────────────────────────────────────────────────── */

static inline t3_vec3 vsub(t3_vec3 a, t3_vec3 b) { return (t3_vec3){ a.x - b.x, a.y - b.y, a.z - b.z }; }
static inline t3_vec3 vadd(t3_vec3 a, t3_vec3 b) { return (t3_vec3){ a.x + b.x, a.y + b.y, a.z + b.z }; }
static inline t3_vec3 vmul(t3_vec3 a, float s) { return (t3_vec3){ a.x * s, a.y * s, a.z * s }; }
static inline float vlen(t3_vec3 a) { return sqrtf(a.x * a.x + a.y * a.y + a.z * a.z); }
static inline t3_quat qmul(t3_quat a, t3_quat b) {
  return (t3_quat){ a.w * b.x + a.x * b.w + a.y * b.z - a.z * b.y, a.w * b.y - a.x * b.z + a.y * b.w + a.z * b.x,
                    a.w * b.z + a.x * b.y - a.y * b.x + a.z * b.w, a.w * b.w - a.x * b.x - a.y * b.y - a.z * b.z };
}
static inline t3_quat qconj(t3_quat q) { return (t3_quat){ -q.x, -q.y, -q.z, q.w }; }
static inline t3_quat qnorm(t3_quat q) {
  float l = sqrtf(q.x * q.x + q.y * q.y + q.z * q.z + q.w * q.w);
  if (l == 0) return (t3_quat){ 0, 0, 0, 1 };
  l = 1 / l;
  return (t3_quat){ q.x * l, q.y * l, q.z * l, q.w * l };
}

/* ── slots ──────────────────────────────────────────────────────────── */

static const char *const slot_names[CW_HB_COUNT] = {
  "hips", "spine", "chest", "upper_chest", "neck", "head",
  "left_shoulder", "left_upper_arm", "left_lower_arm", "left_hand",
  "right_shoulder", "right_upper_arm", "right_lower_arm", "right_hand",
  "left_upper_leg", "left_lower_leg", "left_foot", "left_toes",
  "right_upper_leg", "right_lower_leg", "right_foot", "right_toes",
  "left_thumb_1", "left_thumb_2", "left_thumb_3", "left_index_1", "left_index_2", "left_index_3",
  "left_middle_1", "left_middle_2", "left_middle_3", "left_ring_1", "left_ring_2", "left_ring_3",
  "left_little_1", "left_little_2", "left_little_3",
  "right_thumb_1", "right_thumb_2", "right_thumb_3", "right_index_1", "right_index_2", "right_index_3",
  "right_middle_1", "right_middle_2", "right_middle_3", "right_ring_1", "right_ring_2", "right_ring_3",
  "right_little_1", "right_little_2", "right_little_3",
};

const char *cw_humanoid_bone_name(cw_humanoid_bone slot) { return slot >= 0 && slot < CW_HB_COUNT ? slot_names[slot] : NULL; }
int cw_humanoid_bone_find(const char *name) {
  for (int i = 0; name && i < CW_HB_COUNT; i++)
    if (!strcmp(slot_names[i], name)) return i;
  return -1;
}

/* ── names to slots ─────────────────────────────────────────────────── */

/* A bone name split into lowercase words (at separators, camelCase humps and
 * letter/digit changes), with rig prefixes and the side word taken out:
 * "mixamorig:LeftUpLeg" -> side L, "upleg"; "thigh_twist_01_l" -> side L,
 * "thightwist", number 1; "J_Bip_L_UpperLeg" -> side L, "upperleg". */
typedef struct { char core[64]; int side; /* 0 none, 1 left, 2 right */ int number; /* trailing number, -1 none */ } split_name;

static bool is_noise(const char *w) {
  static const char *const noise[] = { "mixamorig", "mixamo", "bip", "bip01", "bip001", "biped", "j", "c", "def", "org", "orig",
                                       "valvebiped", "armature", "bone", "jnt", "joint", "rig", "sec", "character", "char", "b" };
  for (size_t i = 0; i < sizeof noise / sizeof noise[0]; i++)
    if (!strcmp(w, noise[i])) return true;
  return false;
}

static void split(const char *name, split_name *o) {
  memset(o, 0, sizeof *o);
  o->number = -1;
  char words[16][32];
  int nw = 0, wl = 0;
  char prev = 0;
  for (const char *c = name; ; c++) {
    char ch = *c;
    bool up = ch >= 'A' && ch <= 'Z', lo = ch >= 'a' && ch <= 'z', dg = ch >= '0' && ch <= '9';
    bool pup = prev >= 'A' && prev <= 'Z', plo = prev >= 'a' && prev <= 'z', pdg = prev >= '0' && prev <= '9';
    /* a new word at: a separator, lower->Upper, letter<->digit, and the last capital of a run before a
     * lowercase letter ("LArm" is not split there: one-letter sides in front are handled as a word of 1) */
    bool brk = !(up || lo || dg) || (up && plo) || (dg && (plo || pup)) || ((up || lo) && pdg) ||
               (up && pup && c[1] >= 'a' && c[1] <= 'z' && wl > 1);
    if (brk && wl) {
      words[nw][wl] = 0;
      if (nw < 15) nw++;
      wl = 0;
    }
    if (!ch) break;
    if (up || lo || dg) {
      if (wl < 31) words[nw][wl++] = up ? (char)(ch + 32) : ch;
    }
    prev = ch;
  }
  /* "LArm", "RLeg": a capital L or R glued in front of a capitalised word */
  size_t cl = 0;
  for (int i = 0; i < nw; i++) {
    const char *w = words[i];
    if (!strcmp(w, "l") || !strcmp(w, "left")) { o->side = 1; continue; }
    if (!strcmp(w, "r") || !strcmp(w, "right")) { o->side = 2; continue; }
    if (w[0] >= '0' && w[0] <= '9') { o->number = atoi(w); continue; }
    if (is_noise(w)) continue;
    size_t l = strlen(w);
    if (cl + l < sizeof o->core) { memcpy(o->core + cl, w, l); cl += l; }
  }
  o->core[cl] = 0;
  /* "leftarm" written as one lowercase word */
  if (!o->side && cl > 4 && !strncmp(o->core, "left", 4)) { o->side = 1; memmove(o->core, o->core + 4, cl - 3); }
  else if (!o->side && cl > 5 && !strncmp(o->core, "right", 5)) { o->side = 2; memmove(o->core, o->core + 5, cl - 4); }
}

typedef struct { const char *core; int slot; /* left-side or centre slot */ } alias;
/* centre bones */
static const alias centre_alias[] = {
  { "hips", CW_HB_HIPS }, { "hip", CW_HB_HIPS }, { "pelvis", CW_HB_HIPS },
  { "abdomen", CW_HB_SPINE }, { "waist", CW_HB_SPINE },
  { "chest", CW_HB_CHEST }, { "torso", CW_HB_CHEST },
  { "upperchest", CW_HB_UPPER_CHEST },
  { "neck", CW_HB_NECK }, { "head", CW_HB_HEAD },
};
/* sided bones: the slot is the left one; the right one is at the same offset from its side's first */
static const alias side_alias[] = {
  { "shoulder", CW_HB_LEFT_SHOULDER }, { "clavicle", CW_HB_LEFT_SHOULDER }, { "collar", CW_HB_LEFT_SHOULDER },
  { "upperarm", CW_HB_LEFT_UPPER_ARM }, { "arm", CW_HB_LEFT_UPPER_ARM }, { "uparm", CW_HB_LEFT_UPPER_ARM },
  { "lowerarm", CW_HB_LEFT_LOWER_ARM }, { "forearm", CW_HB_LEFT_LOWER_ARM }, { "elbow", CW_HB_LEFT_LOWER_ARM },
  { "hand", CW_HB_LEFT_HAND }, { "wrist", CW_HB_LEFT_HAND },
  { "upperleg", CW_HB_LEFT_UPPER_LEG }, { "upleg", CW_HB_LEFT_UPPER_LEG }, { "thigh", CW_HB_LEFT_UPPER_LEG },
  { "lowerleg", CW_HB_LEFT_LOWER_LEG }, { "leg", CW_HB_LEFT_LOWER_LEG }, { "calf", CW_HB_LEFT_LOWER_LEG },
  { "shin", CW_HB_LEFT_LOWER_LEG }, { "knee", CW_HB_LEFT_LOWER_LEG },
  { "foot", CW_HB_LEFT_FOOT }, { "ankle", CW_HB_LEFT_FOOT },
  { "toes", CW_HB_LEFT_TOES }, { "toe", CW_HB_LEFT_TOES }, { "toebase", CW_HB_LEFT_TOES }, { "ball", CW_HB_LEFT_TOES },
};
static const char *const finger_names[5][2] = { { "thumb", NULL }, { "index", NULL }, { "middle", NULL }, { "ring", NULL }, { "little", "pinky" } };

static int right_of(int left_slot) {
  if (left_slot >= CW_HB_LEFT_SHOULDER && left_slot <= CW_HB_LEFT_HAND) return left_slot + (CW_HB_RIGHT_SHOULDER - CW_HB_LEFT_SHOULDER);
  if (left_slot >= CW_HB_LEFT_UPPER_LEG && left_slot <= CW_HB_LEFT_TOES) return left_slot + (CW_HB_RIGHT_UPPER_LEG - CW_HB_LEFT_UPPER_LEG);
  if (left_slot >= CW_HB_LEFT_THUMB_1 && left_slot <= CW_HB_LEFT_LITTLE_3) return left_slot + (CW_HB_RIGHT_THUMB_1 - CW_HB_LEFT_THUMB_1);
  return left_slot;
}

/* a finger joint: "index1", "handindex1" (Mixamo), "index01" (number split off), "indexproximal" (VRM),
 * "thumbmetacarpal"; -1 when the core is not a finger */
static int finger_slot(const split_name *s) {
  const char *c = s->core;
  if (!strncmp(c, "hand", 4) && c[4]) c += 4;
  if (!strncmp(c, "finger", 6) && c[6]) c += 6;
  for (int f = 0; f < 5; f++)
    for (int a = 0; a < 2 && finger_names[f][a]; a++) {
      size_t l = strlen(finger_names[f][a]);
      if (strncmp(c, finger_names[f][a], l)) continue;
      const char *rest = c + l;
      int joint = -1;
      if (!*rest) joint = s->number >= 1 && s->number <= 3 ? s->number - 1 : s->number == 0 ? 0 : s->number < 0 ? 0 : -1;
      else if (!strcmp(rest, "proximal")) joint = f == 0 ? 1 : 0;
      else if (!strcmp(rest, "metacarpal")) joint = f == 0 ? 0 : -1;
      else if (!strcmp(rest, "intermediate")) joint = 1;
      else if (!strcmp(rest, "distal")) joint = 2;
      if (joint < 0) return -1;
      return CW_HB_LEFT_THUMB_1 + f * 3 + joint;
    }
  return -1;
}

static void put(cw_bone_map *m, int slot, int bone) {
  if (m->bone[slot] < 0) m->bone[slot] = bone; /* the first in pose order (nearest the root) wins */
}

int cw_bone_map_auto(cw_bone_map *m, const cw_pose *p) {
  for (int i = 0; i < CW_HB_COUNT; i++) m->bone[i] = -1;
  if (!p) return 0;
  /* spine bones numbered in a row ("Spine", "Spine1", "Spine2"; "spine_01".."spine_03") */
  int spine_bone[8], spine_num[8], ns = 0;
  for (int i = 0; i < p->count; i++) {
    if (!p->name[i]) continue;
    split_name s;
    split(p->name[i], &s);
    if (!s.core[0]) continue;
    if (!s.side) {
      if (!strcmp(s.core, "spine")) {
        if (ns < 8) { spine_bone[ns] = i; spine_num[ns++] = s.number; }
        continue;
      }
      if (s.number > 1) continue; /* "neck_02", "head2": not the first of its kind */
      for (size_t a = 0; a < sizeof centre_alias / sizeof centre_alias[0]; a++)
        if (!strcmp(s.core, centre_alias[a].core)) { put(m, centre_alias[a].slot, i); break; }
      continue;
    }
    int f = finger_slot(&s);
    if (f >= 0) { put(m, s.side == 1 ? f : right_of(f), i); continue; }
    if (s.number > 1) continue; /* twist and roll bones are named differently; a second "arm_02" is not the arm */
    for (size_t a = 0; a < sizeof side_alias / sizeof side_alias[0]; a++)
      if (!strcmp(s.core, side_alias[a].core)) { put(m, s.side == 1 ? side_alias[a].slot : right_of(side_alias[a].slot), i); break; }
  }
  /* numbered spine bones in number order: spine, chest, upper chest (names like Chest win where present) */
  for (int a = 0; a < ns; a++)
    for (int b = a + 1; b < ns; b++)
      if (spine_num[b] < spine_num[a]) {
        int t = spine_num[a]; spine_num[a] = spine_num[b]; spine_num[b] = t;
        t = spine_bone[a]; spine_bone[a] = spine_bone[b]; spine_bone[b] = t;
      }
  static const int spine_slots[3] = { CW_HB_SPINE, CW_HB_CHEST, CW_HB_UPPER_CHEST };
  for (int a = 0, k = 0; a < ns && k < 3; a++) {
    while (k < 3 && m->bone[spine_slots[k]] >= 0 && k > 0) k++; /* a named Chest keeps its slot */
    if (k < 3) {
      if (k == 0 && m->bone[CW_HB_SPINE] >= 0) { /* "Abdomen" took spine: numbered ones follow */
        k = 1;
        a--;
        continue;
      }
      m->bone[spine_slots[k++]] = spine_bone[a];
    }
  }
  int n = 0;
  for (int i = 0; i < CW_HB_COUNT; i++) n += m->bone[i] >= 0;
  return n;
}

bool cw_bone_map_set(cw_bone_map *m, const cw_pose *p, const char *slot, const char *bone) {
  int s = cw_humanoid_bone_find(slot);
  if (s < 0) { cw_error("bone map: no humanoid slot named '%s' (hips, spine, chest, left_upper_arm, right_foot, ...)", slot ? slot : ""); return false; }
  if (!bone || !*bone) { m->bone[s] = -1; return true; }
  int b = cw_pose_find(p, bone);
  if (b < 0) { cw_error("bone map: %s = %s: the skeleton has no bone of that name", slot, bone); return false; }
  m->bone[s] = b;
  return true;
}

bool cw_bone_map_apply_text(cw_bone_map *m, const cw_pose *p, const char *text) {
  int lineno = 0;
  for (const char *s = text ? text : ""; *s;) {
    const char *e = strchr(s, '\n');
    size_t len = e ? (size_t)(e - s) : strlen(s);
    char line[256];
    lineno++;
    size_t n = len < sizeof line - 1 ? len : sizeof line - 1;
    memcpy(line, s, n);
    line[n] = 0;
    s += e ? len + 1 : len;
    char *h = strchr(line, '#');
    if (h) *h = 0;
    char *eq = strchr(line, '=');
    char *k = line;
    while (*k == ' ' || *k == '\t') k++;
    if (!*k || *k == '\r') continue;
    if (!eq) { cw_error("bone map line %d: expected `slot = BoneName`, got '%s'", lineno, k); return false; }
    *eq = 0;
    char *ke = eq;
    while (ke > k && (ke[-1] == ' ' || ke[-1] == '\t')) *--ke = 0;
    char *v = eq + 1;
    while (*v == ' ' || *v == '\t') v++;
    size_t vl = strlen(v);
    while (vl && (v[vl - 1] == ' ' || v[vl - 1] == '\t' || v[vl - 1] == '\r')) v[--vl] = 0;
    if (!cw_bone_map_set(m, p, k, v)) { cw_error("bone map line %d", lineno); return false; }
  }
  return true;
}

char *cw_bone_map_to_text(const cw_bone_map *m, const cw_pose *p) {
  size_t cap = 64;
  for (int i = 0; i < CW_HB_COUNT; i++)
    if (m->bone[i] >= 0 && m->bone[i] < p->count) cap += strlen(slot_names[i]) + strlen(p->name[m->bone[i]] ? p->name[m->bone[i]] : "") + 8;
  char *out = cw_alloc(cap, CW_MEM_ANIM);
  size_t at = 0;
  out[0] = 0;
  for (int i = 0; i < CW_HB_COUNT; i++)
    if (m->bone[i] >= 0 && m->bone[i] < p->count)
      at += (size_t)snprintf(out + at, cap - at, "%s = %s\n", slot_names[i], p->name[m->bone[i]] ? p->name[m->bone[i]] : "");
  return out;
}

bool cw_bone_map_usable(const cw_bone_map *m, char *missing, int cap) {
  static const int need[] = { CW_HB_HIPS, CW_HB_LEFT_UPPER_LEG, CW_HB_LEFT_LOWER_LEG, CW_HB_LEFT_FOOT,
                              CW_HB_RIGHT_UPPER_LEG, CW_HB_RIGHT_LOWER_LEG, CW_HB_RIGHT_FOOT };
  bool ok = true;
  size_t at = 0;
  if (missing && cap > 0) missing[0] = 0;
  for (size_t i = 0; i < sizeof need / sizeof need[0]; i++)
    if (m->bone[need[i]] < 0) {
      ok = false;
      if (missing && cap > 0 && at + strlen(slot_names[need[i]]) + 2 < (size_t)cap)
        at += (size_t)snprintf(missing + at, (size_t)cap - at, "%s%s", at ? " " : "", slot_names[need[i]]);
    }
  return ok;
}

/* ── the pairing ────────────────────────────────────────────────────── */

struct cw_retarget {
  cw_retarget_opts o;
  int src_count, dst_count;
  int src_of[CW_HB_COUNT], dst_of[CW_HB_COUNT]; /* bone per slot, both mapped; else -1 */
  int *slot_of_dst;      /* per dst bone: its slot, -1 */
  t3_quat k[CW_HB_COUNT];/* dst model rotation = src model rotation * k */
  float scale;           /* hips and feet */
  float arm_scale;
  t3_vec3 src_hips_rest, dst_hips_rest;      /* model positions at rest */
  float foot_lift[2];    /* dst rest foot height - scale * src rest foot height */
  float foot_side[2];    /* dst rest foot x - scale * src rest foot x: the target keeps its own stance width */
  float leg_len[2];      /* dst, hip joint to ankle along the bones */
  float foot_err[2];
};

static const int dir_child[CW_HB_COUNT] = {
  [CW_HB_HIPS] = -1, [CW_HB_SPINE] = CW_HB_CHEST, [CW_HB_CHEST] = CW_HB_NECK, [CW_HB_UPPER_CHEST] = CW_HB_NECK,
  [CW_HB_NECK] = CW_HB_HEAD, [CW_HB_HEAD] = -1,
  [CW_HB_LEFT_SHOULDER] = CW_HB_LEFT_UPPER_ARM, [CW_HB_LEFT_UPPER_ARM] = CW_HB_LEFT_LOWER_ARM,
  [CW_HB_LEFT_LOWER_ARM] = CW_HB_LEFT_HAND, [CW_HB_LEFT_HAND] = CW_HB_LEFT_MIDDLE_1,
  [CW_HB_RIGHT_SHOULDER] = CW_HB_RIGHT_UPPER_ARM, [CW_HB_RIGHT_UPPER_ARM] = CW_HB_RIGHT_LOWER_ARM,
  [CW_HB_RIGHT_LOWER_ARM] = CW_HB_RIGHT_HAND, [CW_HB_RIGHT_HAND] = CW_HB_RIGHT_MIDDLE_1,
  [CW_HB_LEFT_UPPER_LEG] = CW_HB_LEFT_LOWER_LEG, [CW_HB_LEFT_LOWER_LEG] = CW_HB_LEFT_FOOT,
  [CW_HB_LEFT_FOOT] = CW_HB_LEFT_TOES, [CW_HB_LEFT_TOES] = -1,
  [CW_HB_RIGHT_UPPER_LEG] = CW_HB_RIGHT_LOWER_LEG, [CW_HB_RIGHT_LOWER_LEG] = CW_HB_RIGHT_FOOT,
  [CW_HB_RIGHT_FOOT] = CW_HB_RIGHT_TOES, [CW_HB_RIGHT_TOES] = -1,
};
static int finger_dir_child(int slot) {
  if (slot < CW_HB_LEFT_THUMB_1) return dir_child[slot];
  int k = (slot - CW_HB_LEFT_THUMB_1) % 3;
  return k < 2 ? slot + 1 : -1;
}

/* a copy of a pose's skeleton in its rest pose, model transforms current */
static void rest_copy(cw_pose *out, const cw_pose *p) {
  cw_pose_init(out, p->count);
  for (int i = 0; i < p->count; i++) {
    out->parent[i] = p->parent[i];
    out->pos[i] = out->rest_pos[i] = p->rest_pos[i];
    out->rot[i] = out->rest_rot[i] = p->rest_rot[i];
    out->scl[i] = out->rest_scl[i] = p->rest_scl[i];
    out->base[i] = p->base[i];
    out->base_rot[i] = p->base_rot[i];
  }
  cw_pose_update_model(out);
}

static float chain_len(const cw_pose *p, int a, int b, int c) {
  return vlen(vsub(cw_pose_model_position(p, b), cw_pose_model_position(p, a))) +
         vlen(vsub(cw_pose_model_position(p, c), cw_pose_model_position(p, b)));
}

cw_retarget *cw_retarget_new(const cw_pose *src, const cw_bone_map *sm, const cw_pose *dst, const cw_bone_map *dm,
                             const cw_retarget_opts *opts) {
  char miss[256];
  if (!src || !dst || !sm || !dm) return NULL;
  if (!cw_bone_map_usable(sm, miss, sizeof miss)) { cw_error("cw_retarget_new: the source's bone map has no %s (set them with cw_bone_map_apply_text)", miss); return NULL; }
  if (!cw_bone_map_usable(dm, miss, sizeof miss)) { cw_error("cw_retarget_new: the target's bone map has no %s (set them with cw_bone_map_apply_text)", miss); return NULL; }
  cw_retarget *r = cw_calloc(1, sizeof *r, CW_MEM_ANIM);
  r->o = opts ? *opts : CW_RETARGET_OPTS_DEFAULT;
  r->src_count = src->count;
  r->dst_count = dst->count;
  r->slot_of_dst = cw_calloc((size_t)(dst->count ? dst->count : 1), sizeof(int), CW_MEM_ANIM);
  for (int i = 0; i < dst->count; i++) r->slot_of_dst[i] = -1;
  cw_pose sr, dr;
  rest_copy(&sr, src);
  rest_copy(&dr, dst);
  for (int s = 0; s < CW_HB_COUNT; s++) {
    bool both = sm->bone[s] >= 0 && sm->bone[s] < src->count && dm->bone[s] >= 0 && dm->bone[s] < dst->count;
    r->src_of[s] = both ? sm->bone[s] : -1;
    r->dst_of[s] = both ? dm->bone[s] : -1;
    if (both) r->slot_of_dst[dm->bone[s]] = s;
  }
  /* rest alignment per slot: the turn that takes the target's rest direction (bone to the next mapped bone
   * along the limb) onto the source's. A bone at the end of a limb has no direction of its own (a hand without
   * finger bones, the head, the toes): it takes the alignment of the bone it hangs from, so a hand stays in
   * line with its forearm when one rig rests in a T-pose and the other in an A-pose. */
  t3_quat align[CW_HB_COUNT];
  bool has_align[CW_HB_COUNT];
  for (int s = 0; s < CW_HB_COUNT; s++) {
    align[s] = (t3_quat){ 0, 0, 0, 1 };
    has_align[s] = false;
    if (r->src_of[s] < 0) continue;
    int c = finger_dir_child(s);
    bool leg = (s >= CW_HB_LEFT_UPPER_LEG && s <= CW_HB_LEFT_TOES) || (s >= CW_HB_RIGHT_UPPER_LEG && s <= CW_HB_RIGHT_TOES);
    if (r->o.align_rest && (!leg || r->o.align_legs) && c >= 0 && r->src_of[c] >= 0) {
      t3_vec3 ds = vsub(cw_pose_model_position(&sr, r->src_of[c]), cw_pose_model_position(&sr, r->src_of[s]));
      t3_vec3 dd = vsub(cw_pose_model_position(&dr, r->dst_of[c]), cw_pose_model_position(&dr, r->dst_of[s]));
      float ls = vlen(ds), ld = vlen(dd);
      if (ls > 1e-6f && ld > 1e-6f) { align[s] = cw_quat_from_to(vmul(dd, 1 / ld), vmul(ds, 1 / ls)); has_align[s] = true; }
    }
  }
  for (int s = 0; s < CW_HB_COUNT; s++) { /* slots are listed from the body outwards, so a parent is done first */
    int from = s == CW_HB_HEAD ? CW_HB_NECK : s == CW_HB_LEFT_HAND ? CW_HB_LEFT_LOWER_ARM : s == CW_HB_RIGHT_HAND ? CW_HB_RIGHT_LOWER_ARM
             : s == CW_HB_LEFT_FOOT ? CW_HB_LEFT_LOWER_LEG : s == CW_HB_RIGHT_FOOT ? CW_HB_RIGHT_LOWER_LEG
             : s == CW_HB_LEFT_TOES ? CW_HB_LEFT_FOOT : s == CW_HB_RIGHT_TOES ? CW_HB_RIGHT_FOOT
             : s >= CW_HB_LEFT_THUMB_1 && (s - CW_HB_LEFT_THUMB_1) % 3 ? s - 1 : -1;
    if (r->src_of[s] >= 0 && !has_align[s] && from >= 0 && has_align[from]) { align[s] = align[from]; has_align[s] = true; }
  }
  for (int s = 0; s < CW_HB_COUNT; s++) {
    r->k[s] = (t3_quat){ 0, 0, 0, 1 };
    if (r->src_of[s] < 0) continue;
    /* dst = (src * src_rest^-1) * align * dst_rest */
    r->k[s] = qnorm(qmul(qconj(cw_pose_model_rotation(&sr, r->src_of[s])), qmul(align[s], cw_pose_model_rotation(&dr, r->dst_of[s]))));
  }
  float sl = 0, dl = 0;
  for (int side = 0; side < 2; side++) {
    int u = side ? CW_HB_RIGHT_UPPER_LEG : CW_HB_LEFT_UPPER_LEG;
    sl += chain_len(&sr, r->src_of[u], r->src_of[u + 1], r->src_of[u + 2]);
    r->leg_len[side] = chain_len(&dr, r->dst_of[u], r->dst_of[u + 1], r->dst_of[u + 2]);
    dl += r->leg_len[side];
  }
  r->scale = r->o.hips_scale > 0 ? r->o.hips_scale : sl > 1e-6f ? dl / sl : 1;
  for (int side = 0; side < 2; side++) {
    int f = side ? CW_HB_RIGHT_FOOT : CW_HB_LEFT_FOOT;
    r->foot_lift[side] = cw_pose_model_position(&dr, r->dst_of[f]).y - r->scale * cw_pose_model_position(&sr, r->src_of[f]).y;
    r->foot_side[side] = cw_pose_model_position(&dr, r->dst_of[f]).x - r->scale * cw_pose_model_position(&sr, r->src_of[f]).x;
  }
  r->arm_scale = 1;
  if (r->src_of[CW_HB_LEFT_HAND] >= 0 && r->src_of[CW_HB_LEFT_UPPER_ARM] >= 0 && r->src_of[CW_HB_LEFT_LOWER_ARM] >= 0) {
    float sa = chain_len(&sr, r->src_of[CW_HB_LEFT_UPPER_ARM], r->src_of[CW_HB_LEFT_LOWER_ARM], r->src_of[CW_HB_LEFT_HAND]);
    float da = chain_len(&dr, r->dst_of[CW_HB_LEFT_UPPER_ARM], r->dst_of[CW_HB_LEFT_LOWER_ARM], r->dst_of[CW_HB_LEFT_HAND]);
    if (sa > 1e-6f) r->arm_scale = da / sa;
  }
  r->src_hips_rest = cw_pose_model_position(&sr, r->src_of[CW_HB_HIPS]);
  r->dst_hips_rest = cw_pose_model_position(&dr, r->dst_of[CW_HB_HIPS]);
  cw_pose_free(&sr);
  cw_pose_free(&dr);
  return r;
}

void cw_retarget_free(cw_retarget *r) {
  if (!r) return;
  cw_free(r->slot_of_dst);
  cw_free(r);
}

float cw_retarget_scale(const cw_retarget *r) { return r ? r->scale : 1; }
float cw_retarget_foot_error(const cw_retarget *r, int side) { return r && side >= 0 && side < 2 ? r->foot_err[side] : 0; }

t3_vec3 cw_retarget_foot_target(const cw_retarget *r, const cw_pose *src, int side) {
  if (!r || !src || src->count != r->src_count) return (t3_vec3){ 0, 0, 0 };
  side = side ? 1 : 0;
  t3_vec3 sp = cw_pose_model_position(src, r->src_of[side ? CW_HB_RIGHT_FOOT : CW_HB_LEFT_FOOT]);
  return (t3_vec3){ sp.x * r->scale + r->foot_side[side], sp.y * r->scale + r->foot_lift[side], sp.z * r->scale };
}

void cw_retarget_apply(const cw_retarget *rc, const cw_pose *src, cw_pose *dst) { cw_retarget_apply_feet(rc, src, dst, NULL); }

void cw_retarget_apply_feet(const cw_retarget *rc, const cw_pose *src, cw_pose *dst, const t3_vec3 *feet) {
  cw_retarget *r = (cw_retarget *)rc; /* the foot error of the last apply is the only state */
  if (!r || !src || !dst || src->count != r->src_count || dst->count != r->dst_count) return;
  /* 1. rotations: parents first, each mapped bone to its transferred model rotation */
  for (int i = 0; i < dst->count; i++) {
    int s = r->slot_of_dst[i], par = dst->parent[i];
    t3_quat pm = par >= 0 ? dst->mrot[par] : dst->base_rot[i];
    dst->pos[i] = dst->rest_pos[i];
    dst->scl[i] = dst->rest_scl[i];
    if (s >= 0) {
      t3_quat want = qmul(src->mrot[r->src_of[s]], r->k[s]);
      dst->rot[i] = qnorm(qmul(qconj(pm), want));
    } else {
      dst->rot[i] = dst->rest_rot[i];
    }
    dst->mrot[i] = qnorm(qmul(pm, dst->rot[i]));
  }
  cw_pose_update_model(dst);
  /* 2. the hips travel by the source's travel from rest, scaled */
  int sh = r->src_of[CW_HB_HIPS], dh = r->dst_of[CW_HB_HIPS];
  t3_vec3 want = vadd(r->dst_hips_rest, vmul(vsub(cw_pose_model_position(src, sh), r->src_hips_rest), r->scale));
  cw_pose_translate_model(dst, dh, vsub(want, cw_pose_model_position(dst, dh)));
  /* 3. feet */
  t3_vec3 target[2];
  for (int side = 0; side < 2; side++) target[side] = feet ? feet[side] : cw_retarget_foot_target(r, src, side);
  if (r->o.foot_ik) {
    /* a leg that cannot reach its target lowers the hips until it can (the lower of the two needs) */
    float drop = 0;
    for (int side = 0; side < 2; side++) {
      int u = side ? CW_HB_RIGHT_UPPER_LEG : CW_HB_LEFT_UPPER_LEG;
      t3_vec3 v = vsub(cw_pose_model_position(dst, r->dst_of[u]), target[side]);
      float reach = r->leg_len[side] * 0.9995f, horiz2 = v.x * v.x + v.z * v.z, r2 = reach * reach;
      if (v.y > 0 && vlen(v) > reach && horiz2 < r2) {
        float d = v.y - sqrtf(r2 - horiz2);
        if (d > drop) drop = d;
      }
    }
    /* never deeper than half a leg: a stride longer than the target's legs is left short instead */
    float cap = 0.5f * (r->leg_len[0] < r->leg_len[1] ? r->leg_len[0] : r->leg_len[1]);
    if (drop > cap) drop = cap;
    if (drop > 0) cw_pose_translate_model(dst, dh, (t3_vec3){ 0, -drop, 0 });
    for (int side = 0; side < 2; side++) {
      int u = side ? CW_HB_RIGHT_UPPER_LEG : CW_HB_LEFT_UPPER_LEG;
      int a = r->dst_of[u], b = r->dst_of[u + 1], c = r->dst_of[u + 2];
      if (!cw_pose_is_descendant(dst, b, a) || !cw_pose_is_descendant(dst, c, b)) continue;
      cw_ik2_opts o = CW_IK2_OPTS_DEFAULT;
      o.use_end_rotation = true;
      o.end_rotation = cw_pose_model_rotation(dst, c); /* the foot keeps the rotation it was given */
      cw_ik_two_bone(dst, a, b, c, target[side], &o);
    }
  }
  for (int side = 0; side < 2; side++) {
    int f = side ? CW_HB_RIGHT_FOOT : CW_HB_LEFT_FOOT;
    r->foot_err[side] = vlen(vsub(cw_pose_model_position(dst, r->dst_of[f]), target[side]));
  }
  /* 4. hands, from the chest */
  if (r->o.hand_ik) {
    int anchor = r->src_of[CW_HB_CHEST] >= 0 ? CW_HB_CHEST : r->src_of[CW_HB_SPINE] >= 0 ? CW_HB_SPINE : CW_HB_HIPS;
    t3_vec3 sa = cw_pose_model_position(src, r->src_of[anchor]), da = cw_pose_model_position(dst, r->dst_of[anchor]);
    for (int side = 0; side < 2; side++) {
      int u = side ? CW_HB_RIGHT_UPPER_ARM : CW_HB_LEFT_UPPER_ARM;
      int a = r->dst_of[u], b = r->dst_of[u + 1], c = r->dst_of[u + 2];
      if (a < 0 || b < 0 || c < 0 || !cw_pose_is_descendant(dst, b, a) || !cw_pose_is_descendant(dst, c, b)) continue;
      t3_vec3 t = vadd(da, vmul(vsub(cw_pose_model_position(src, r->src_of[u + 2]), sa), r->arm_scale));
      cw_ik2_opts o = CW_IK2_OPTS_DEFAULT;
      o.use_end_rotation = true;
      o.end_rotation = cw_pose_model_rotation(dst, c);
      cw_ik_two_bone(dst, a, b, c, t, &o);
    }
  }
}

/* ── planted feet ───────────────────────────────────────────────────── */

static float smooth01(float x) {
  x = x < 0 ? 0 : x > 1 ? 1 : x;
  return x * x * (3 - 2 * x);
}

t3_vec3 cw_foot_plant(t3_vec3 *left, t3_vec3 *right, const float *times, int count, float length, const cw_foot_plant_opts *opts) {
  cw_foot_plant_opts o = opts ? *opts : CW_FOOT_PLANT_OPTS_DEFAULT;
  t3_vec3 vel = { 0, 0, 0 };
  if (!left || !right || !times || count < 3) return vel;
  t3_vec3 *foot[2] = { left, right };
  /* frames are a cycle when the clip ends where it starts: the last frame repeats the first */
  int n = count - 1; /* distinct frames of the cycle (frame n is frame 0 again) */
  bool *planted[2];
  for (int f = 0; f < 2; f++) {
    float lo = foot[f][0].y, hi = lo;
    for (int k = 1; k < count; k++) { if (foot[f][k].y < lo) lo = foot[f][k].y; if (foot[f][k].y > hi) hi = foot[f][k].y; }
    planted[f] = cw_calloc((size_t)count, 1, CW_MEM_ANIM);
    float limit = lo + o.height * (hi - lo);
    /* a foot that never leaves the ground (a clip standing still) is planted throughout */
    for (int k = 0; k < count; k++) planted[f][k] = foot[f][k].y <= limit || hi - lo < 1e-6f;
  }
  /* the mean velocity of planted feet (frame to frame, both planted) */
  double sx = 0, sz = 0, st = 0;
  for (int f = 0; f < 2; f++)
    for (int k = 0; k + 1 < count; k++)
      if (planted[f][k] && planted[f][k + 1]) {
        sx += (double)foot[f][k + 1].x - (double)foot[f][k].x;
        sz += (double)foot[f][k + 1].z - (double)foot[f][k].z;
        st += (double)times[k + 1] - (double)times[k];
      }
  if (st > 0) { vel.x = (float)(sx / st); vel.z = (float)(sz / st); }
  if (!o.adjust) { cw_free(planted[0]); cw_free(planted[1]); return vel; }
  bool cyclic = o.cyclic;
  for (int f = 0; f < 2; f++) {
    /* the offset of every planted frame: onto the line through its stance's mean position, at the mean velocity */
    t3_vec3 *off = cw_calloc((size_t)count, sizeof *off, CW_MEM_ANIM);
    bool all = true;
    for (int k = 0; k < n; k++) all &= planted[f][k];
    /* stances: runs of planted frames, joined across the end of a cycle */
    int start = 0;
    if (cyclic && !all)
      while (planted[f][start]) start++; /* begin in a swing so no stance is cut in two */
    for (int i = 0; i < n;) {
      int k = cyclic ? (start + i) % n : i;
      if (!planted[f][k]) { i++; continue; }
      int run = 0;
      while (i + run < n && planted[f][cyclic ? (start + i + run) % n : i + run]) run++;
      if (!cyclic && i + run == n && planted[f][n]) run++; /* the last frame of a clip that does not repeat */
      /* time unwrapped along the run */
      double mt = 0, mx = 0, mz = 0;
      for (int j = 0; j < run; j++) {
        int q = cyclic ? (start + i + j) % n : i + j;
        double t = (double)times[q] + (cyclic && start + i + j >= n ? (double)length : 0);
        mt += t; mx += foot[f][q].x; mz += foot[f][q].z;
      }
      mt /= run; mx /= run; mz /= run;
      for (int j = 0; j < run; j++) {
        int q = cyclic ? (start + i + j) % n : i + j;
        double t = (double)times[q] + (cyclic && start + i + j >= n ? (double)length : 0);
        off[q].x = (float)(mx + (double)vel.x * (t - mt)) - foot[f][q].x;
        off[q].z = (float)(mz + (double)vel.z * (t - mt)) - foot[f][q].z;
      }
      i += run;
    }
    if (cyclic) off[n] = off[0];
    /* swing frames ease out of the stance before them and into the one after, over o.blend seconds */
    for (int k = 0; k < count; k++) {
      if (planted[f][k] || o.blend <= 0) continue;
      t3_vec3 add = { 0, 0, 0 };
      for (int dir = -1; dir <= 1; dir += 2) {
        for (int j = 1; j < n; j++) {
          int q = k + dir * j;
          double wrap = 0;
          if (cyclic) { while (q < 0) { q += n; wrap -= length; } while (q >= n) { q -= n; wrap += length; } }
          else if (q < 0 || q >= count) break;
          double dt = fabs((double)times[q] + wrap - (double)times[k]);
          if (dt >= o.blend) break;
          if (planted[f][q]) {
            float w = smooth01(1 - (float)(dt / o.blend));
            add.x += off[q].x * w;
            add.z += off[q].z * w;
            break;
          }
        }
      }
      foot[f][k].x += add.x;
      foot[f][k].z += add.z;
    }
    for (int k = 0; k < count; k++)
      if (planted[f][k]) { foot[f][k].x += off[k].x; foot[f][k].z += off[k].z; }
    if (cyclic) foot[f][n] = foot[f][0];
    cw_free(off);
  }
  cw_free(planted[0]);
  cw_free(planted[1]);
  return vel;
}

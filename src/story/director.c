/* director.c: the cinematic camera (cartwheel/director.h). Ported from keyholder's director.c
 * (github.com/virgilvox/keyholder, MIT, see CREDITS), itself after DOLLY's director.js: candidate placements per shot
 * type, scored for clear sightlines, the face toward the lens, nobody's hair or hands across the face, the 180-degree
 * line and cut variety; a live re-score that re-solves near the lens and eases or cuts. Changed here: subjects and the
 * world come from the game (no actor or world module), there is no handheld noise, drift, dutch roll or randomness
 * (the camera never shakes, and the same inputs give the same frames), and every frame is measured for the gates. */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cartwheel/director.h"
#include "cartwheel/debug.h"

typedef t3_vec3 v3;
static v3 V(float x, float y, float z) { return (v3){ x, y, z }; }
static v3 add(v3 a, v3 b) { return V(a.x + b.x, a.y + b.y, a.z + b.z); }
static v3 sub(v3 a, v3 b) { return V(a.x - b.x, a.y - b.y, a.z - b.z); }
static v3 scl(v3 a, float s) { return V(a.x * s, a.y * s, a.z * s); }
static float dot(v3 a, v3 b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
static v3 cross(v3 a, v3 b) { return V(a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x); }
static float len(v3 a) { return sqrtf(dot(a, a)); }
static v3 norm(v3 a) { float l = len(a); return l > 1e-6f ? scl(a, 1 / l) : V(0, 0, 1); }
static v3 lerp3(v3 a, v3 b, float t) { return add(a, scl(sub(b, a), t)); }
static float clampf(float x, float a, float b) { return x < a ? a : x > b ? b : x; }
static float lerpf(float a, float b, float t) { return a + (b - a) * t; }
static float smooth(float k) { k = clampf(k, 0, 1); return k * k * (3 - 2 * k); }
static float wrap_pi(float a) { while (a > 3.14159265f) a -= 6.2831853f; while (a < -3.14159265f) a += 6.2831853f; return a; }
static float seg_dist(v3 p, v3 a, v3 b) { v3 ab = sub(b, a); float t = clampf(dot(sub(p, a), ab) / fmaxf(1e-5f, dot(ab, ab)), 0, 1); return len(sub(p, add(a, scl(ab, t)))); }
#define PI_F 3.14159265f

typedef struct { v3 pos, look; float fov; } frame_t;
typedef struct { float ang, dk, hk, base; int side; } cand_t;

struct cw_director {
  cw_director_desc d;
  cw_dir_subject subj[CW_DIR_MAX_SUBJECTS]; bool has[CW_DIR_MAX_SUBJECTS];
  v3 vel[CW_DIR_MAX_SUBJECTS], last_eye[CW_DIR_MAX_SUBJECTS]; bool vel_init[CW_DIR_MAX_SUBJECTS];
  cw_shot s, asked; cand_t c; float t; bool active, fresh, retry;
  v3 pos, look; float fov;
  float check_t, since, score0; int bad;
  v3 e_pos, e_look; float e_fov, e_t, e_len; bool whip;
  bool releasing; float rel_t, rel_len; cw_dir_camera game; bool have_game;
  int line_side, line_a, line_b; bool have_line;
  v3 last_pos, last_look; float last_fov; bool have_last; cw_shot_type last_type;
  /* measures */
  v3 m_p0, m_v0; int m_n; uint32_t noise_rng; float m_time, m_rev_at[3];
  int32_t dbg_active, dbg_type, dbg_solved, resolves, cuts, blocked_frames, away_frames, out_frames;
  float dbg_score, key_x, key_y, block_radius;
  int32_t reversals;
};

static const cw_dir_subject *S_(const cw_director *d, int i) { return i >= 0 && i < CW_DIR_MAX_SUBJECTS && d->has[i] ? &d->subj[i] : NULL; }
static float hrad(const cw_dir_subject *s) { return s->head_radius > 0 ? s->head_radius : .14f; }
static v3 eye(const cw_dir_subject *s) { return s->eye; }
static v3 chest(const cw_dir_subject *s) { return s->chest; }
static v3 hand(const cw_dir_subject *s, bool left) { v3 h = left ? s->hand_l : s->hand_r; return h.x == 0 && h.y == 0 && h.z == 0 ? s->chest : h; }
static v3 face_dir(const cw_dir_subject *s) {
  v3 f = s->face; f.y = 0;
  if (len(f) < 1e-3f) { f = sub(s->eye, s->chest); f.y = 0; }
  if (len(f) < 1e-3f) f = V(0, 0, 1);
  return norm(f);
}
static bool ray(const cw_director *d, v3 a, v3 b, float *t) { float tt = 1; if (!d->d.ray) return false; bool h = d->d.ray(a, b, &tt, d->d.user); if (t) *t = tt; return h; }
static bool visible(const cw_director *d, v3 a, v3 b) { float t; return !ray(d, a, b, &t) || t > .98f; }
static bool near_wall(const cw_director *d, v3 p, float r) {
  static const float D[6][3] = { { 1, 0, 0 }, { -1, 0, 0 }, { 0, 0, 1 }, { 0, 0, -1 }, { 0, 1, 0 }, { 0, -1, 0 } };
  for (int i = 0; i < 6; i++) { float t; if (ray(d, p, add(p, V(D[i][0] * r, D[i][1] * r, D[i][2] * r)), &t)) return true; }
  return false;
}
static int nsubj(const cw_director *d, const cw_shot *s) { int n = 0; for (int i = 0; i < s->nsubj && i < 3; i++) if (S_(d, s->subj[i])) n++; return n; }
static const cw_dir_subject *sub_at(const cw_director *d, const cw_shot *s, int k) { return k < s->nsubj ? S_(d, s->subj[k]) : NULL; }
static v3 center(const cw_director *d, const cw_shot *s) {
  v3 c = V(0, 0, 0); int n = 0;
  for (int i = 0; i < s->nsubj; i++) { const cw_dir_subject *x = sub_at(d, s, i); if (x) { c = add(c, x->feet); n++; } }
  return n ? scl(c, 1.f / (float)n) : c;
}
static bool close_type(cw_shot_type t) { return t == CW_SHOT_MCU || t == CW_SHOT_CU || t == CW_SHOT_PUSH || t == CW_SHOT_REACT || t == CW_SHOT_HERO; }
static v3 insert_pt(const cw_director *d, const cw_shot *s) {
  const cw_dir_subject *A = sub_at(d, s, 0);
  if (s->part == CW_SHOT_PART_POINT || !A) return s->point;
  if (s->part == CW_SHOT_PART_HEAD) return eye(A);
  if (s->part == CW_SHOT_PART_CHEST) return chest(A);
  return hand(A, s->part == CW_SHOT_PART_HAND_L);
}
static v3 key_of(const cw_director *d, const cw_shot *s) {
  const cw_dir_subject *A = sub_at(d, s, 0), *B = sub_at(d, s, 1);
  if (s->type == CW_SHOT_INSERT) return insert_pt(d, s);
  if (s->type == CW_SHOT_OTS && B) return eye(B);
  if (s->type == CW_SHOT_TOP) { v3 c = center(d, s); c.y += .5f; return c; }
  if (s->type == CW_SHOT_WIDE || s->type == CW_SHOT_ESTABLISH || s->type == CW_SHOT_TWO) { v3 c = center(d, s); c.y += 1.2f; return c; }
  return A ? eye(A) : s->point;
}
static float face_yaw(const cw_director *d, const cw_shot *s) {
  const cw_dir_subject *A = sub_at(d, s, 0), *B = sub_at(d, s, 1);
  if (!A) return 0;
  v3 fd = face_dir(A);
  if (B) { v3 x = sub(B->feet, A->feet); x.y = 0; if (dot(x, x) > .01f && dot(norm(x), fd) > .2f) return atan2f(x.x, x.z); }
  return atan2f(fd.x, fd.z);
}

static frame_t frame_of(const cw_director *d, const cw_shot *s, const cand_t *c, float t) {
  frame_t f = { V(0, 0, 0), V(0, 0, 1), 40 };
  float ang = c->ang, dk = c->dk, hk = c->hk; int side = c->side;
  const cw_dir_subject *A = sub_at(d, s, 0), *B = sub_at(d, s, 1);
  switch (s->type) {
    case CW_SHOT_WIDE: case CW_SHOT_ESTABLISH: {
      v3 ce = center(d, s); bool e = s->type == CW_SHOT_ESTABLISH;
      float dd = (s->dist > 0 ? s->dist : (e ? 9 : 4.6f)) * dk, h = (s->height > 0 ? s->height : (e ? 3.2f : 1.75f)) + hk;
      f.pos = V(ce.x + sinf(ang) * dd, ce.y + h, ce.z + cosf(ang) * dd);
      f.look = V(ce.x, ce.y + 1.15f, ce.z); f.fov = s->fov > 0 ? s->fov : (e ? 50 : 40);
      break;
    }
    case CW_SHOT_TWO: {
      v3 a = A ? A->feet : V(0, 0, 0), b = B ? B->feet : a;
      float ly = atan2f(b.x - a.x, b.z - a.z), yaw = ly + (float)side * PI_F / 2 + ang;
      v3 ce = scl(add(a, b), .5f);
      float dd = ((s->dist > 0 ? s->dist : 2.5f) + len(sub(b, a)) * .8f) * dk;
      f.pos = V(ce.x + sinf(yaw) * dd, ce.y + 1.38f + hk, ce.z + cosf(yaw) * dd);
      f.look = V(ce.x, ce.y + 1.2f, ce.z); f.fov = s->fov > 0 ? s->fov : 34;
      break;
    }
    case CW_SHOT_OTS: {
      v3 a = A ? A->feet : V(0, 0, 0), b = B ? B->feet : add(a, V(0, 0, 1));
      v3 dir = sub(b, a); dir.y = 0; dir = norm(dir);
      v3 right = V(dir.z, 0, -dir.x), ea = A ? eye(A) : a, eb = B ? eye(B) : b;
      f.pos = add(add(ea, scl(dir, -dk * .95f)), scl(right, (float)side * (.62f + hk)));
      f.pos.y = ea.y + .02f - hk * .3f;
      f.look = add(eb, scl(right, (float)side * .2f)); f.look.y -= .08f;
      f.fov = s->fov > 0 ? s->fov : 27;
      break;
    }
    case CW_SHOT_MCU: case CW_SHOT_CU: case CW_SHOT_REACT: case CW_SHOT_PUSH: {
      if (!A) break;
      float near = s->dist > 0 ? s->dist : s->type == CW_SHOT_CU ? 1.25f : s->type == CW_SHOT_REACT ? 1.35f : 1.5f;
      if (s->type == CW_SHOT_PUSH) {
        float d0 = s->dist > 0 ? s->dist : 2.7f, k = smooth(t / (s->dur > 0 ? s->dur : 4));
        near = lerpf(d0, 1.2f, k * k * .3f + k * .7f);
      }
      float fy = c->base, y = fy + ang; v3 e = eye(A);
      f.pos = V(e.x + sinf(y) * near * dk, e.y - .05f + hk, e.z + cosf(y) * near * dk);
      v3 lead = V(sinf(fy), 0, cosf(fy));
      bool tight = s->type == CW_SHOT_CU || s->type == CW_SHOT_REACT;
      f.look = add(e, scl(lead, tight ? .04f : .1f)); f.look.y = e.y - (tight ? .03f : .14f);
      f.fov = s->fov > 0 ? s->fov : (s->type == CW_SHOT_CU ? 24 : s->type == CW_SHOT_REACT ? 25 : s->type == CW_SHOT_PUSH ? 31 : 30);
      break;
    }
    case CW_SHOT_HERO: {
      if (!A) break;
      v3 p = A->feet, e = eye(A); float y = c->base + ang, dd = (s->dist > 0 ? s->dist : 2.3f) * dk;
      float ht = e.y - p.y;
      f.pos = V(e.x + sinf(y) * dd, p.y + (s->height > 0 ? s->height : .18f * ht) + hk, e.z + cosf(y) * dd);
      f.look = V(e.x, e.y - .17f * ht, e.z); f.fov = s->fov > 0 ? s->fov : 50;
      break;
    }
    case CW_SHOT_INSERT: {
      v3 q = insert_pt(d, s); float y = c->base + ang, dd = (s->dist > 0 ? s->dist : .85f) * dk;
      f.pos = V(q.x + sinf(y) * dd, q.y + .45f + hk, q.z + cosf(y) * dd);
      f.look = q; f.fov = s->fov > 0 ? s->fov : 32;
      break;
    }
    case CW_SHOT_TOP: {
      v3 ce = center(d, s); float spread = 0;
      for (int i = 0; i < s->nsubj; i++) { const cw_dir_subject *x = sub_at(d, s, i); if (x) spread = fmaxf(spread, len(sub(x->feet, ce))); }
      float h = fmaxf(s->height > 0 ? s->height : 5.5f, spread * 1.2f + 2.4f) * dk, off = .35f + hk;
      f.pos = V(ce.x + sinf(ang) * off, ce.y + h, ce.z + cosf(ang) * off);
      f.look = V(ce.x, ce.y + .5f, ce.z); f.fov = s->fov > 0 ? s->fov : 54 + fmaxf(0, 1 - dk) * 40;
      break;
    }
    default: break;
  }
  return f;
}

static float min_dist(cw_shot_type t) {
  switch (t) {
    case CW_SHOT_CU: case CW_SHOT_REACT: return .85f;
    case CW_SHOT_MCU: case CW_SHOT_PUSH: return 1.f;
    case CW_SHOT_OTS: return 1.25f;
    case CW_SHOT_INSERT: return .4f;
    case CW_SHOT_HERO: return 1.3f;
    default: return 1.4f;
  }
}
static void basis(frame_t f, v3 *fw, v3 *rt, v3 *up) {
  *fw = norm(sub(f.look, f.pos));
  *rt = norm(cross(*fw, fabsf(fw->y) > .98f ? V(0, 0, 1) : V(0, 1, 0)));
  *up = cross(*rt, *fw);
}
static bool is_subject(const cw_shot *s, int idx, int upto) { for (int k = 0; k < upto && k < s->nsubj; k++) if (s->subj[k] == idx) return true; return false; }

/* how good a framing is; fail: it must not be used (or, live, must change now) */
static float rate(const cw_director *d, const cw_shot *s, frame_t f, bool *fail) {
  *fail = false;
  if (near_wall(d, f.pos, .2f)) { *fail = true; return -100; }
  float aspect = d->d.aspect > 0 ? d->d.aspect : 16.f / 9;
  v3 key = key_of(d, s);
  const cw_dir_subject *A = sub_at(d, s, 0);
  bool closeup = close_type(s->type);
  /* clear view of what the shot is about */
  float got = 0, n = 0;
  if (s->type == CW_SHOT_INSERT) { n = 2; got = (float)visible(d, f.pos, key) + (float)visible(d, f.pos, add(key, V(0, .08f, 0))); }
  else {
    int i0 = s->type == CW_SHOT_OTS && s->nsubj > 1 ? 1 : 0, i1 = closeup || s->type == CW_SHOT_OTS ? i0 + 1 : s->nsubj;
    for (int i = i0; i < i1 && i < s->nsubj; i++) { const cw_dir_subject *x = sub_at(d, s, i); if (!x) continue;
      n += 2; got += (float)visible(d, f.pos, eye(x)) + (float)visible(d, f.pos, chest(x)); }
  }
  float v = n > 0 ? got / n : 1;
  float sc = v * 3 - (v < .6f ? 2 : 0);
  if (v < .5f) *fail = true;
  v3 fw, rt, up; basis(f, &fw, &rt, &up);
  float th = tanf(f.fov * PI_F / 360), kz = dot(sub(key, f.pos), fw);
  for (int i = 0; i < CW_DIR_MAX_SUBJECTS; i++) {
    const cw_dir_subject *o = S_(d, i);
    if (!o || !o->visible) continue;
    bool subj = is_subject(s, i, closeup || s->type == CW_SHOT_INSERT ? 1 : s->nsubj);
    bool shoulder = s->type == CW_SHOT_OTS && s->nsubj > 0 && i == s->subj[0];
    if (s->type == CW_SHOT_INSERT && s->part == CW_SHOT_PART_POINT) subj = false;
    v3 h = eye(o), hair = add(h, V(0, hrad(o), 0)), c2 = V(h.x, h.y - .5f, h.z), c3 = add(o->feet, V(0, .15f, 0));
    float near_cam = fminf(seg_dist(f.pos, hair, c3), len(sub(f.pos, h)));
    if (s->type == CW_SHOT_INSERT && subj) { if (near_cam < .3f) { *fail = true; return -100; } continue; }
    if (near_cam < (shoulder ? .4f : .65f) && (!subj || shoulder)) { *fail = true; return -100; }
    if (subj && !shoulder) {
      if (!closeup && s->type != CW_SHOT_INSERT && s->type != CW_SHOT_TOP) {
        if (near_cam < 1.5f) sc -= (1.5f - near_cam) * 3;
        v3 fd = face_dir(o), tc = sub(f.pos, h); tc.y = 0; tc = norm(tc);
        if (dot(fd, tc) < -.3f) sc -= near_cam < 2.5f ? 1.5f : .5f;
      }
      continue;
    }
    /* another head, its hair, or a body across the line to the face */
    float r = hrad(o);
    float blk = fminf(fminf(seg_dist(h, f.pos, key), seg_dist(hair, f.pos, key)), seg_dist(c2, f.pos, key));
    if (blk < (shoulder ? .3f : r + .2f)) { sc -= shoulder ? 2.5f : 2.2f; if (!shoulder && blk < r + .08f) *fail = true; }
    else if (blk < .5f && !shoulder) sc -= .4f;
    if (shoulder) continue;
    if ((closeup || s->type == CW_SHOT_OTS) && s->nsubj > 1 && i == s->subj[1]) continue; /* a dirty single: theirs only the face line */
    int fg = 0; float zn = 1e9f;
    v3 pts[4] = { hair, c2, V(h.x, h.y - .9f, h.z), add(c3, V(0, .3f, 0)) };
    for (int k = 0; k < 4; k++) {
      v3 dd = sub(pts[k], f.pos); float z = dot(dd, fw);
      if (z < .15f || z > kz - .3f) continue;
      float sx = dot(dd, rt) / z / (th * aspect), sy = dot(dd, up) / z / th;
      if (fabsf(sx) < 1.05f && fabsf(sy) < 1.05f) { fg++; zn = fminf(zn, z); }
    }
    if (fg) {
      sc -= (float)fg * .5f + (zn < 1.6f ? 1.5f : 0) + (zn < 1.2f ? 2 : 0);
      if (fg >= 2 && ((zn < 1.2f && (closeup || s->type == CW_SHOT_INSERT || s->type == CW_SHOT_OTS)) || zn < .9f)) *fail = true;
    }
  }
  /* the subject's own hands (and a blade held up from one) off the face */
  if ((closeup || s->type == CW_SHOT_OTS) && A) {
    const cw_dir_subject *F = s->type == CW_SHOT_OTS && sub_at(d, s, 1) ? sub_at(d, s, 1) : A;
    v3 kd = sub(key, f.pos); float kl = len(kd);
    v3 k2 = add(f.pos, scl(kd, fmaxf(0, kl - .3f) / fmaxf(.01f, kl)));
    for (int k = 0; k < 2; k++) {
      v3 hp = hand(F, k == 1);
      float dd = fminf(seg_dist(hp, f.pos, k2), fminf(seg_dist(add(hp, V(0, .45f, 0)), f.pos, k2), seg_dist(add(hp, V(0, .85f, 0)), f.pos, k2)));
      if (dd < .22f) sc -= 3.5f; else if (dd < .34f) sc -= 1.2f;
    }
  }
  /* a group shot keeps everyone in it */
  if ((s->type == CW_SHOT_TWO || s->type == CW_SHOT_WIDE || s->type == CW_SHOT_TOP || s->type == CW_SHOT_ESTABLISH) && s->nsubj > 1) {
    for (int i = 0; i < s->nsubj; i++) {
      const cw_dir_subject *x = sub_at(d, s, i); if (!x) continue;
      v3 dd = sub(chest(x), f.pos); float z = dot(dd, fw);
      if (z < .3f) { *fail = true; break; }
      float sx = dot(dd, rt) / z / (th * aspect), sy = dot(dd, up) / z / th;
      if (fabsf(sx) > .95f || fabsf(sy) > .95f) { sc -= 2; if (fabsf(sx) > 1.1f || fabsf(sy) > 1.1f) *fail = true; }
    }
  }
  /* a close-up sees a face: never a profile into hair or the back of the head */
  if (closeup && A) {
    v3 fd = face_dir(A), tc = sub(f.pos, eye(A)); tc.y = 0; tc = norm(tc);
    float k = dot(fd, tc), kmin = s->type == CW_SHOT_HERO ? -.05f : .3f;
    if (k < kmin) { *fail = true; sc -= 3; } else if (k < .5f) sc -= (.5f - k) * 3;
  }
  /* a wall between the look point and the lens */
  { v3 dd = sub(f.pos, f.look); float l = len(dd), t;
    if (l > .1f && ray(d, f.look, f.pos, &t) && t * l > .45f && t < 1 - .05f / l) { sc -= 3; if (t < .8f) *fail = true; } }
  /* something solid in front of the lens, across the frame, as far as the subject */
  {
    float reach = fminf(5, fmaxf(.5f, kz - .35f)); int hits = 0;
    for (int i = 0; i < 9; i++) {
      float kx = (float)(i % 3 - 1) * 2.1f, ky = i < 3 ? -1.3f : i < 6 ? 0 : 1.f;
      v3 dd = add(fw, add(scl(rt, kx * th), scl(up, ky * th)));
      float t;
      if (ray(d, f.pos, add(f.pos, scl(dd, reach)), &t) && !(ky < 0 && t * reach > 2)) { hits++; sc -= i == 4 ? 1.5f : t * reach < 1 ? .7f : .4f; }
    }
    if (hits >= 4 && (closeup || s->type == CW_SHOT_OTS || s->type == CW_SHOT_INSERT)) *fail = true;
  }
  /* the key point in frame, not too close, not cut off */
  {
    v3 dd = sub(key, f.pos); float z = dot(dd, fw);
    if (z < min_dist(s->type) * .85f) { *fail = true; sc -= 3; }
    else if (z < min_dist(s->type)) sc -= 1;
    if (z > .1f) {
      float sx = dot(dd, rt) / z / (th * aspect), sy = dot(dd, up) / z / th;
      if (fabsf(sx) > .85f || sy > .8f || sy < -.85f) { *fail = true; sc -= 3; }
      else if (fabsf(sx) > .6f || sy > .6f) sc -= .5f;
      if (close_type(s->type)) sc -= fabsf(sx) * .8f; /* a face held near the middle, a little off for a look room */
    } else *fail = true;
  }
  return sc;
}
/* everyone moved on by their velocity for secs (restored by predict_end): a placement must stay clean as people walk */
static cw_dir_subject PRED_SAVE[CW_DIR_MAX_SUBJECTS];
static bool PRED_OFF; /* a solve where nothing is clean once people move on: then clean now is the best there is */
static bool predict_begin(cw_director *d, float secs) {
  if (PRED_OFF) return false;
  bool moving = false;
  for (int i = 0; i < CW_DIR_MAX_SUBJECTS; i++) if (d->has[i] && len(d->vel[i]) > .3f) moving = true;
  if (!moving) return false;
  memcpy(PRED_SAVE, d->subj, sizeof PRED_SAVE);
  for (int i = 0; i < CW_DIR_MAX_SUBJECTS; i++) {
    if (!d->has[i]) continue;
    v3 o = scl(d->vel[i], secs);
    cw_dir_subject *x = &d->subj[i];
    x->eye = add(x->eye, o); x->chest = add(x->chest, o); x->feet = add(x->feet, o);
    if (len(x->hand_l) > 0) x->hand_l = add(x->hand_l, o);
    if (len(x->hand_r) > 0) x->hand_r = add(x->hand_r, o);
  }
  return true;
}
static void predict_end(cw_director *d) { memcpy(d->subj, PRED_SAVE, sizeof PRED_SAVE); }
static float rate_shot(const cw_director *d_, const cw_shot *s, const cand_t *c, float t, bool *fail) {
  cw_director *d = (cw_director *)d_;
  float sc = rate(d, s, frame_of(d, s, c, t), fail);
  if (*fail) return sc;
  /* where everyone will be in a moment: a passer-by about to cross the line fails the placement now */
  for (int k = 1; k <= 2; k++) {
    if (!predict_begin(d, .3f * (float)k)) break;
    bool f2; float s2 = rate(d, s, frame_of(d, s, c, t), &f2);
    predict_end(d);
    if (f2) { *fail = true; return s2; }
    sc = fminf(sc, s2 + .5f);
  }
  float te = s->type == CW_SHOT_PUSH ? (s->dur > 0 ? s->dur : 4) : -1;
  if (te > t) { bool f2; float s2 = rate(d, s, frame_of(d, s, c, te), &f2); if (f2) { *fail = true; return s2; } sc = fminf(sc, s2); }
  return sc;
}

static float fb_best; static cand_t fb_c;
static void try_cand(cw_director *d, const cw_shot *s, cand_t c, float prefer, float t, const v3 *near_pos, float *best, cand_t *bc) {
  bool fail;
  float sc = rate_shot(d, s, &c, t, &fail);
  if (fail) { if (sc > -100 && sc + prefer > fb_best) { fb_best = sc + prefer; fb_c = c; } return; }
  sc += prefer;
  frame_t f = frame_of(d, s, &c, t);
  const cw_dir_subject *A = sub_at(d, s, 0), *B = sub_at(d, s, 1);
  if (A && B && s->type != CW_SHOT_INSERT && d->have_line) {
    v3 a = A->feet, b = B->feet;
    int sd = (b.x - a.x) * (f.pos.z - a.z) - (b.z - a.z) * (f.pos.x - a.x) > 0 ? 1 : -1;
    bool same = d->line_a == s->subj[0] && d->line_b == s->subj[1], flip = d->line_a == s->subj[1] && d->line_b == s->subj[0];
    if ((same && sd != d->line_side) || (flip && sd == d->line_side)) sc -= .9f;
  }
  if (near_pos) sc -= fminf(3, len(sub(*near_pos, f.pos))) * .35f;
  else if (d->have_last) {
    v3 d1 = norm(sub(d->last_look, d->last_pos)), d2 = norm(sub(f.look, f.pos));
    float a = acosf(clampf(dot(d1, d2), -1, 1));
    if (a < .35f && len(sub(d->last_pos, f.pos)) < 1.5f) sc -= .6f;
  }
  if (sc > *best) { *best = sc; *bc = c; }
}
static cand_t C(float ang, float dk, float hk, int side, float base) { return (cand_t){ ang, dk, hk, base, side }; }

static float solve(cw_director *d, cw_shot *s, cand_t *bc, float t, const v3 *near_pos) {
  float best = -1e9f;
  *bc = C(0, 1, 0, 1, 0);
  float fy = s->nsubj ? face_yaw(d, s) : 0;
  if (d->d.no_solve) {
    /* the control: the type's first placement, never scored */
    *bc = s->type == CW_SHOT_INSERT ? C(.45f, 1, 0, 1, sub_at(d, s, 0) ? atan2f(face_dir(sub_at(d, s, 0)).x, face_dir(sub_at(d, s, 0)).z) : 0) : C(0, 1, 0, 1, fy);
    return 0;
  }
  switch (s->type) {
    case CW_SHOT_WIDE: case CW_SHOT_ESTABLISH:
      for (int r = 0; r < 24; r++) { float y = (float)r * PI_F / 12;
        try_cand(d, s, C(y, 1, 0, 1, 0), 0, t, near_pos, &best, bc);
        try_cand(d, s, C(y, .75f, 0, 1, 0), -.1f, t, near_pos, &best, bc);
        try_cand(d, s, C(y, .55f, .2f, 1, 0), -.25f, t, near_pos, &best, bc);
        try_cand(d, s, C(y, 1, .9f, 1, 0), -.15f, t, near_pos, &best, bc); }
      break;
    case CW_SHOT_TWO: {
      static const float off[7] = { -.9f, -.6f, -.3f, 0, .3f, .6f, .9f }, dks[3] = { 1, .75f, 1.3f };
      for (int k = 0; k < 3; k++) for (int o = 0; o < 7; o++) for (int sd = -1; sd <= 1; sd += 2)
        try_cand(d, s, C(off[o], dks[k], 0, sd, 0), -fabsf(off[o]) * .15f - fabsf(dks[k] - 1) * .3f, t, near_pos, &best, bc);
      break;
    }
    case CW_SHOT_OTS: {
      static const float hks[3] = { 0, .18f, .34f };
      for (int sd = -1; sd <= 1; sd += 2) for (int h = 0; h < 3; h++) {
        try_cand(d, s, C(0, 1, hks[h], sd, 0), -(float)h * .2f, t, near_pos, &best, bc);
        try_cand(d, s, C(0, 1.35f, hks[h], sd, 0), -.05f - (float)h * .2f, t, near_pos, &best, bc);
      }
      break;
    }
    case CW_SHOT_MCU: case CW_SHOT_CU: case CW_SHOT_REACT: case CW_SHOT_PUSH: case CW_SHOT_HERO: {
      static const float angs[11] = { -.75f, -.5f, -.25f, 0, .25f, .5f, .75f, -1.05f, 1.05f, -1.4f, 1.4f }, dks[3] = { 1, .8f, 1.3f };
      int na = s->type == CW_SHOT_REACT ? 7 : 11;
      for (int h = 0; h < 2; h++) for (int k = 0; k < 3; k++) for (int a = 0; a < na; a++)
        try_cand(d, s, C(angs[a], dks[k], (float)h * .32f, 1, fy), -fabsf(angs[a]) * .25f + (fabsf(angs[a]) > .1f ? .05f : 0) - fabsf(dks[k] - 1) * .4f - (float)h * .5f, t, near_pos, &best, bc);
      break;
    }
    case CW_SHOT_INSERT: {
      const cw_dir_subject *A = sub_at(d, s, 0);
      float by = A ? atan2f(face_dir(A).x, face_dir(A).z) : 0;
      if (s->part == CW_SHOT_PART_POINT && A) { v3 x = sub(A->feet, s->point); by = atan2f(x.x, x.z); }
      static const float angs[8] = { .45f, -.45f, .8f, -.8f, 0, 1.2f, -1.2f, PI_F };
      for (int a = 0; a < 8; a++) for (int k = 0; k < 2; k++)
        try_cand(d, s, C(angs[a], k ? 1.3f : 1, k ? .15f : 0, 1, by), -fabsf(angs[a]) * .12f - (float)k * .15f, t, near_pos, &best, bc);
      break;
    }
    case CW_SHOT_TOP:
      for (int r = 0; r < 8; r++) for (int k = 0; k < 6; k++) {
        static const float hs[6] = { 1, .75f, .58f, .47f, .47f, .47f }, os[6] = { 0, 0, .5f, .9f, 1.7f, 2.6f };
        try_cand(d, s, C((float)r * PI_F / 4, hs[k], os[k], 1, 0), -(float)k * .2f, t, near_pos, &best, bc);
      }
      break;
    default: break;
  }
  return best;
}
static float solve_any_(cw_director *d, cw_shot *s, cand_t *bc, float t, const v3 *near_pos);
static float solve_any(cw_director *d, cw_shot *s, cand_t *bc, float t, const v3 *near_pos) {
  cw_shot s0 = *s;
  float best = solve_any_(d, s, bc, t, near_pos);
  if (best < -1 && !PRED_OFF && !d->d.no_solve) {
    cw_shot s2 = s0; cand_t c2;
    PRED_OFF = true;
    float b2 = solve_any_(d, &s2, &c2, t, near_pos);
    PRED_OFF = false;
    if (b2 > best) { best = b2; *s = s2; *bc = c2; }
  }
  return best;
}
static float solve_any_(cw_director *d, cw_shot *s, cand_t *bc, float t, const v3 *near_pos) {
  fb_best = -1e9f;
  cw_shot s0 = *s;
  float best = solve(d, s, bc, t, near_pos);
  if (d->d.no_solve) return best;
  float fb0 = fb_best; cand_t fc0 = fb_c;
  if (best > -50 && best < .3f && s->type != CW_SHOT_WIDE && s->type != CW_SHOT_ESTABLISH) {
    cw_shot_type alt[3]; int na = 0;
    if (close_type(s->type) && s->type != CW_SHOT_MCU) alt[na++] = CW_SHOT_MCU;
    if (s->nsubj >= 2 && s->type != CW_SHOT_TWO && s->type != CW_SHOT_INSERT) alt[na++] = CW_SHOT_TWO;
    alt[na++] = CW_SHOT_WIDE;
    for (int i = 0; i < na; i++) {
      cw_shot m = *s; m.type = alt[i]; m.fov = 0;
      if (alt[i] == CW_SHOT_WIDE) { m.dist = 3.6f; m.height = 1.6f; } else m.dist = 0;
      cand_t c2; float b2 = solve(d, &m, &c2, t, near_pos);
      if (b2 > best + 1.5f) { best = b2; *bc = c2; *s = m; }
    }
    return best;
  }
  if (best < -50) {
    if (close_type(s->type) && s->type != CW_SHOT_MCU) { cw_shot m = *s; m.type = CW_SHOT_MCU; m.dist = 0; best = solve(d, &m, bc, t, near_pos); if (best > -50) { *s = m; return best; } }
    cw_shot w = *s; w.type = CW_SHOT_WIDE; w.dist = 3.6f; w.height = 1.6f; w.fov = 0;
    best = solve(d, &w, bc, t, near_pos);
    if (best < -50) { w.dist = 2.4f; best = solve(d, &w, bc, t, near_pos); }
    *s = w;
    if (best < -50) {
      if (fb0 > -50) { *s = s0; *bc = fc0; best = fb0 - 5; }
      else if (fb_best > -50) { *bc = fb_c; best = fb_best - 5; }
      else *bc = C(0, 1, 0, 1, 0);
    }
  }
  return best;
}
static void note_line(cw_director *d) {
  const cw_dir_subject *A = sub_at(d, &d->s, 0), *B = sub_at(d, &d->s, 1);
  if (A && B && d->s.type != CW_SHOT_INSERT) {
    frame_t f = frame_of(d, &d->s, &d->c, d->t);
    v3 a = A->feet, b = B->feet;
    d->line_a = d->s.subj[0]; d->line_b = d->s.subj[1]; d->have_line = true;
    d->line_side = (b.x - a.x) * (f.pos.z - a.z) - (b.z - a.z) * (f.pos.x - a.x) > 0 ? 1 : -1;
  }
}

cw_director *cw_director_new(const cw_director_desc *desc) {
  cw_director *d = calloc(1, sizeof *d);
  if (desc) d->d = *desc;
  if (d->d.ease <= 0) d->d.ease = 5;
  d->last_fov = 40; d->block_radius = 0;
  for (int k = 0; k < 3; k++) d->m_rev_at[k] = -1;
  d->dbg_type = d->dbg_solved = -1;
  return d;
}
void cw_director_free(cw_director *d) { free(d); }
void cw_director_set_subject(cw_director *d, int i, const cw_dir_subject *s) {
  if (!d || i < 0 || i >= CW_DIR_MAX_SUBJECTS) return;
  if (!s) { d->has[i] = false; return; }
  d->subj[i] = *s; d->has[i] = true;
}
void cw_director_set_gameplay(cw_director *d, const cw_dir_camera *cam) { if (d && cam) { d->game = *cam; d->have_game = true; } }

void cw_director_cut(cw_director *d, const cw_shot *in) {
  if (!d || !in) return;
  if (in->type == CW_SHOT_RELEASE) { cw_director_release(d, in->dur > 0 ? in->dur : .6f); return; }
  cw_shot s = *in;
  bool whip = s.type == CW_SHOT_WHIP && d->active;
  if (s.type == CW_SHOT_WHIP) { s.type = nsubj(d, &s) == 2 ? CW_SHOT_TWO : nsubj(d, &s) > 2 ? CW_SHOT_WIDE : CW_SHOT_MCU; }
  /* an insert on a hand hanging at the hip would frame the hips: it needs a hand raised to work height */
  const cw_dir_subject *A = sub_at(d, &s, 0);
  if (s.type == CW_SHOT_INSERT && A && s.part != CW_SHOT_PART_POINT && s.part != CW_SHOT_PART_HEAD && s.part != CW_SHOT_PART_CHEST) {
    v3 q = insert_pt(d, &s);
    if (q.y - A->feet.y < .8f * (eye(A).y - A->feet.y)) { s.type = CW_SHOT_MCU; s.nsubj = 1; s.dist = 0; s.fov = 0; }
  }
  /* a close-up on someone walking hangs back */
  if (close_type(s.type) && A && len(d->vel[s.subj[0]]) > .6f && !(s.dist > 0)) s.dist = 2.4f;
  cand_t bc;
  cw_shot asked = s;
  float sc = solve_any(d, &s, &bc, 0, NULL);
  d->asked = asked; d->retry = s.type != asked.type;
  if (in->type == CW_SHOT_WHIP && s.type == CW_SHOT_MCU) bc.dk = fmaxf(bc.dk, 1.15f);
  bool was_active = d->active && !d->releasing;
  d->s = s; d->c = bc; d->t = 0; d->fresh = !whip; d->check_t = 0; d->since = 0; d->bad = 0; d->score0 = sc;
  d->whip = whip; d->e_t = 0; d->e_len = 0; d->releasing = false;
  if (whip) { d->e_pos = d->last_pos; d->e_look = d->last_look; d->e_fov = d->last_fov; d->e_len = .42f; }
  else if (!was_active && d->have_game) { /* in from the game's camera: a cut (a cinematic starts on a cut) */ }
  d->active = true; d->cuts++;
  d->dbg_type = (int32_t)in->type; d->dbg_solved = (int32_t)s.type; d->dbg_score = sc;
  d->last_type = in->type;
  note_line(d);
}
void cw_director_cut_story(cw_director *d, const cw_story_shot *ss, const int *map) {
  if (!d || !ss) return;
  cw_shot s = { .type = ss->type, .nsubj = ss->nsubj, .dur = ss->dur, .dist = ss->dist, .fov = ss->fov, .height = ss->height, .part = ss->part };
  for (int i = 0; i < 3; i++) s.subj[i] = ss->subj[i] < 0 ? -1 : map ? map[ss->subj[i]] : ss->subj[i];
  cw_director_cut(d, &s);
}
void cw_director_release(cw_director *d, float seconds) {
  if (!d || !d->active) return;
  d->releasing = true; d->rel_t = 0; d->rel_len = seconds > 0 ? seconds : .01f;
  d->e_pos = d->pos; d->e_look = d->look; d->e_fov = d->fov;
}
void cw_director_stop(cw_director *d) { if (d) { d->active = false; d->releasing = false; d->whip = false; } }
bool cw_director_active(const cw_director *d) { return d && d->active; }
bool cw_director_shot_done(const cw_director *d) { return !d || !d->active || (d->s.dur > 0 && d->t >= d->s.dur - 1e-3f); }
float cw_director_shot_time(const cw_director *d) { return d ? d->t : 0; }
float cw_director_whip(const cw_director *d) {
  if (!d || !d->active || !d->whip || d->e_len <= 0 || d->e_t >= d->e_len) return 0;
  return sinf(PI_F * clampf(d->e_t / d->e_len, 0, 1));
}
cw_shot_type cw_director_solved_type(const cw_director *d) { return d ? d->s.type : CW_SHOT_WIDE; }

static void track(cw_director *d, float dt) {
  if (dt <= 0) return;
  for (int i = 0; i < CW_DIR_MAX_SUBJECTS; i++) {
    if (!d->has[i]) { d->vel_init[i] = false; continue; }
    v3 p = d->subj[i].eye;
    if (!d->vel_init[i]) { d->vel_init[i] = true; d->last_eye[i] = p; d->vel[i] = V(0, 0, 0); continue; }
    v3 v = scl(sub(p, d->last_eye[i]), 1 / dt); v.y = 0;
    if (len(v) > 8) v = V(0, 0, 0);
    d->vel[i] = lerp3(d->vel[i], v, 1 - expf(-6 * dt)); d->last_eye[i] = p;
  }
}
static void recheck(cw_director *d, float dt) {
  if (d->whip || d->d.no_solve) return;
  d->since += dt; d->check_t += dt;
  if (d->retry && d->t > .12f) {
    d->retry = false;
    cw_shot s = d->asked; cand_t bc; fb_best = -1e9f;
    float nsc = solve(d, &s, &bc, d->t, NULL);
    if (nsc > fmaxf(.5f, d->score0 - 1)) { d->s = s; d->c = bc; d->fresh = true; d->score0 = nsc; d->since = 0; d->bad = 0; d->cuts++; d->dbg_solved = (int32_t)s.type; note_line(d); return; }
  }
  frame_t live = { d->pos, d->look, d->fov };
  bool fail; float sc = rate(d, &d->s, live, &fail);
  /* someone walking into the lens in the next 0.4 s: change now, by a cut */
  bool coming = false;
  if (!fail && d->since >= .4f && predict_begin(d, .4f)) { bool f2; /* (at most one such cut in 0.4 s) */ rate(d, &d->s, live, &f2); predict_end(d); coming = f2; }
  if (coming) { fail = true; sc = fminf(sc, -2); d->bad = 4; d->since = 1; }
  if (!coming && d->check_t < .1f) return;
  d->check_t = 0;
  bool soft = sc < d->score0 - 2.5f && sc < 1.5f;
  if (!coming) d->bad = (fail || soft) ? d->bad + (fail ? 2 : 1) : 0;
  if (d->bad < 4 || (d->since < .9f && !(fail && sc <= -100))) {
    /* someone already across the face: do not wait out the hold */
    if (!(fail && d->bad >= 2 && sc < -1)) return;
  }
  cw_shot s = d->s; cand_t bc; v3 here = d->pos;
  float nsc = solve_any(d, &s, &bc, d->t, &here);
  frame_t f = frame_of(d, &s, &bc, d->t);
  bool path = len(sub(f.pos, here)) < 1.6f && visible(d, here, f.pos);
  for (int i = 0; path && i < CW_DIR_MAX_SUBJECTS; i++) {
    const cw_dir_subject *o = S_(d, i); if (!o || !o->visible) continue;
    v3 h = eye(o);
    if (seg_dist(h, here, f.pos) < .45f || seg_dist(V(h.x, h.y - .6f, h.z), here, f.pos) < .45f) path = false;
  }
  d->s = s; d->c = bc; d->since = 0; d->bad = 0; d->score0 = nsc; d->resolves++; d->dbg_solved = (int32_t)s.type;
  if (path && sc > -100 && !coming) { d->e_pos = d->pos; d->e_look = d->look; d->e_fov = d->fov; d->e_t = 0; d->e_len = .5f; }
  else { d->fresh = true; d->e_len = 0; d->cuts++; }
  note_line(d);
}

/* the measures the gates read: what the live camera sees */
static void measure(cw_director *d, float dt, v3 pos, v3 look, float fov, bool framed) {
  d->m_time += dt;
  /* a cut, a whip pan's designed swing and the ease back to the game's camera (which blends in the game's own motion) are not
   * shake: the count is of the director's shots */
  if (d->fresh || d->whip || d->releasing) d->m_n = 0;
  if (dt > 0) {
    v3 v = scl(sub(pos, d->m_p0), 1 / dt);
    if (d->m_n >= 2) {
      /* a turn back within 0.1 s of the last one on that axis: motion faster than 5 Hz, a shake (a walking subject's
       * head bob, followed at 2 Hz, is not one) */
      const float lo = .01f; float vn[3] = { v.x, v.y, v.z }, vo[3] = { d->m_v0.x, d->m_v0.y, d->m_v0.z };
      bool rev = false;
      for (int k = 0; k < 3; k++) if (vo[k] * vn[k] < 0 && fabsf(vo[k]) > lo && fabsf(vn[k]) > lo) {
        if (d->m_time - d->m_rev_at[k] < .1f) rev = true;
        d->m_rev_at[k] = d->m_time;
      }
      if (rev) d->reversals++;
    }
    d->m_p0 = pos; d->m_v0 = v; d->m_n++;
  }
  if (!framed) return;
  v3 key = key_of(d, &d->s);
  frame_t f = { pos, look, fov }; v3 fw, rt, up; basis(f, &fw, &rt, &up);
  float th = tanf(fov * PI_F / 360), aspect = d->d.aspect > 0 ? d->d.aspect : 16.f / 9;
  v3 kd = sub(key, pos); float z = dot(kd, fw);
  d->key_x = z > .05f ? dot(kd, rt) / z / (th * aspect) : 9; d->key_y = z > .05f ? dot(kd, up) / z / th : 9;
  if (fabsf(d->key_x) > 1 || fabsf(d->key_y) > 1) d->out_frames++;
  bool blocked = !visible(d, pos, key);
  bool closeup = close_type(d->s.type);
  for (int i = 0; i < CW_DIR_MAX_SUBJECTS && !blocked; i++) {
    const cw_dir_subject *o = S_(d, i); if (!o || !o->visible) continue;
    if (is_subject(&d->s, i, closeup || d->s.type == CW_SHOT_INSERT ? 1 : d->s.nsubj)) continue;
    if (d->s.type == CW_SHOT_OTS && d->s.nsubj && i == d->s.subj[0]) continue;
    v3 h = eye(o), hair = add(h, V(0, hrad(o), 0)), body = o->chest;
    /* only what is between the lens and the face */
    float r = d->block_radius > 0 ? d->block_radius : hrad(o);
    float th_ = dot(sub(h, pos), fw);
    if (th_ <= 0 || th_ >= z) continue;
    if (seg_dist(h, pos, key) < r || seg_dist(hair, pos, key) < r || seg_dist(body, pos, key) < (o->radius > 0 ? o->radius : .25f)) blocked = true;
  }
  if (blocked) d->blocked_frames++;
  if (closeup && sub_at(d, &d->s, 0) && d->s.type != CW_SHOT_HERO) {
    const cw_dir_subject *A = sub_at(d, &d->s, 0);
    v3 fd = face_dir(A), tc = sub(pos, eye(A)); tc.y = 0; tc = norm(tc);
    if (dot(fd, tc) < .2f) d->away_frames++;
  }
}

bool cw_director_update(cw_director *d, float dt, cw_dir_camera *cam) {
  if (!d || !d->active) { if (d) d->dbg_active = 0; return false; }
  d->t += dt;
  track(d, dt);
  recheck(d, dt);
  frame_t f = frame_of(d, &d->s, &d->c, d->t);
  /* lead a moving subject by the follow's lag */
  if (d->s.type != CW_SHOT_TOP && d->s.nsubj && d->s.subj[0] >= 0 && d->s.subj[0] < CW_DIR_MAX_SUBJECTS) {
    v3 v = d->vel[d->s.subj[0]];
    f.pos = add(f.pos, scl(v, .2f)); f.look = add(f.look, scl(v, .14f));
  }
  if (d->fresh) { d->pos = f.pos; d->look = f.look; d->fov = f.fov; }
  if (d->e_len > 0 && d->e_t < d->e_len) {
    d->e_t += dt;
    float k = clampf(d->e_t / d->e_len, 0, 1);
    if (d->whip) {
      float ks = k < .5f ? 4 * k * k * k : 1 - powf(-2 * k + 2, 3) / 2;
      d->pos = lerp3(d->e_pos, f.pos, ks);
      v3 a = norm(sub(d->e_look, d->e_pos)), b = norm(sub(f.look, f.pos));
      float y0 = atan2f(a.x, a.z), p0 = asinf(clampf(a.y, -1, 1)), y1 = atan2f(b.x, b.z), p1 = asinf(clampf(b.y, -1, 1));
      float y = y0 + wrap_pi(y1 - y0) * ks, p = lerpf(p0, p1, ks);
      float dl = lerpf(len(sub(d->e_look, d->e_pos)), len(sub(f.look, f.pos)), ks);
      d->look = add(d->pos, scl(V(sinf(y) * cosf(p), sinf(p), cosf(y) * cosf(p)), dl));
      d->fov = lerpf(d->e_fov, f.fov, ks) + sinf(PI_F * k) * 9;
    } else {
      float ks = smooth(k);
      d->pos = lerp3(d->e_pos, f.pos, ks); d->look = lerp3(d->e_look, f.look, ks); d->fov = lerpf(d->e_fov, f.fov, ks);
    }
    if (d->e_t >= d->e_len) d->whip = false;
  } else if (!d->fresh) {
    float r = d->s.type == CW_SHOT_INSERT ? d->d.ease * 1.6f : d->d.ease, kp = 1 - expf(-r * dt), kl = 1 - expf(-r * 1.4f * dt), kf = 1 - expf(-6 * dt);
    d->pos = lerp3(d->pos, f.pos, kp); d->look = lerp3(d->look, f.look, kl); d->fov = lerpf(d->fov, f.fov, kf);
  }
  bool was_fresh = d->fresh;
  v3 pos = d->pos, look = d->look; float fov = d->fov;
  /* the last guard: a wall between the lens and what it looks at pulls the lens in (and asks for a new angle) */
  { float t; v3 dd = sub(pos, look); float l = len(dd);
    if (l > .1f && ray(d, look, pos, &t) && t * l > .45f && t < 1 - 1e-3f) { pos = add(look, scl(dd, t - .14f / l)); if (t < .85f) d->bad += 2; } }
  bool framed = !d->releasing && !d->whip; /* what the measures judge: a shot holding, not a pan or the way out */
  if (d->releasing) {
    d->rel_t += dt;
    float k = smooth(d->rel_t / d->rel_len);
    if (d->have_game) { pos = lerp3(pos, d->game.pos, k); look = lerp3(look, d->game.look, k); fov = lerpf(fov, d->game.fov, k); }
    if (d->rel_t >= d->rel_len) { d->active = false; d->releasing = false; }
  }
  if (d->d.test_noise > 0) { /* a gate's control only: what a handheld shake would add */
    for (int k = 0; k < 3; k++) { d->noise_rng = d->noise_rng * 1664525u + 1013904223u; ((float *)&pos)[k] += ((float)(d->noise_rng >> 8) / 16777216.f - .5f) * 2 * d->d.test_noise; }
  }
  measure(d, dt, pos, look, fov, framed);
  d->fresh = false;
  (void)was_fresh;
  cam->pos = pos; cam->look = look; cam->fov = fov;
  d->last_pos = pos; d->last_look = look; d->last_fov = fov; d->have_last = true;
  d->dbg_active = d->active ? 1 : 0;
  return true;
}

static const char *dname(const char *prefix, const char *leaf) {
  size_t n = strlen(prefix) + strlen(leaf) + 2;
  char *s = malloc(n); snprintf(s, n, "%s.%s", prefix, leaf);
  return s;
}
void cw_director_publish(cw_director *d, const char *prefix) {
  if (!d) return;
  if (!prefix) prefix = "dir";
  cw_debug_i32(dname(prefix, "active"), &d->dbg_active);
  cw_debug_i32(dname(prefix, "type"), &d->dbg_type);
  cw_debug_i32(dname(prefix, "solved"), &d->dbg_solved);
  cw_debug_f32(dname(prefix, "score"), &d->dbg_score);
  cw_debug_i32(dname(prefix, "resolves"), &d->resolves);
  cw_debug_i32(dname(prefix, "cuts"), &d->cuts);
  cw_debug_i32(dname(prefix, "blocked_frames"), &d->blocked_frames);
  cw_debug_i32(dname(prefix, "away_frames"), &d->away_frames);
  cw_debug_i32(dname(prefix, "out_frames"), &d->out_frames);
  cw_debug_i32(dname(prefix, "reversals"), &d->reversals);
  cw_debug_f32(dname(prefix, "key_x"), &d->key_x);
  cw_debug_f32(dname(prefix, "key_y"), &d->key_y);
}

float cw_director_measure(const cw_director *d, cw_dir_measure m) {
  if (!d) return 0;
  switch (m) {
    case CW_DIR_M_BLOCKED: return (float)d->blocked_frames;
    case CW_DIR_M_AWAY: return (float)d->away_frames;
    case CW_DIR_M_OUT: return (float)d->out_frames;
    case CW_DIR_M_REVERSALS: return (float)d->reversals;
    case CW_DIR_M_RESOLVES: return (float)d->resolves;
    case CW_DIR_M_CUTS: return (float)d->cuts;
    case CW_DIR_M_KEY_X: return d->key_x;
    case CW_DIR_M_KEY_Y: return d->key_y;
  }
  return 0;
}

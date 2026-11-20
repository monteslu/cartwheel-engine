/* cw_retarget_clip: a clip for one skeleton baked into a clip for another
 * (cartwheel/retarget.h), through the pose transfer of retarget.c. */
#include <stdio.h>
#include <stdlib.h>

#include "anim_internal.h"
#include "cartwheel/retarget.h"

static void pose_rest(cw_pose *p) {
  for (int i = 0; i < p->count; i++) { p->pos[i] = p->rest_pos[i]; p->rot[i] = p->rest_rot[i]; p->scl[i] = p->rest_scl[i]; }
}

/* per track: the pose bone its bone track names, -1 for anything else; returns how many matched */
static int match_tracks(anim_rec *a, const cw_pose *p, int *bone_of) {
  int matched = 0;
  for (int i = 0; i < a->tracks.count; i++) {
    const anim_track *t = &a->tracks.data[i];
    bone_of[i] = t->kind == CW_TRACK_VALUE && t->bone && t->prop ? cw_pose_find(p, t->bone) : -1;
    matched += bone_of[i] >= 0;
  }
  return matched;
}

/* the clip's pose at `time`: rest, then every bone track; model transforms current */
static void pose_at(anim_rec *a, const int *bone_of, cw_pose *p, float time) {
  pose_rest(p);
  for (int i = 0; i < a->tracks.count; i++) {
    int b = bone_of[i];
    if (b < 0 || !a->tracks.data[i].enabled) continue;
    const anim_track *t = &a->tracks.data[i];
    cw_value v;
    if (!strcmp(t->prop, "quaternion")) {
      if (cw__track_sample(t, time, CW_T_QUAT, &v)) p->rot[b] = (t3_quat){ v.v[0], v.v[1], v.v[2], v.v[3] };
    } else if (!strcmp(t->prop, "position")) {
      if (cw__track_sample(t, time, CW_T_VEC3, &v)) p->pos[b] = (t3_vec3){ v.v[0], v.v[1], v.v[2] };
    } else if (!strcmp(t->prop, "scale")) {
      if (cw__track_sample(t, time, CW_T_VEC3, &v)) p->scl[b] = (t3_vec3){ v.v[0], v.v[1], v.v[2] };
    }
  }
  cw_pose_update_model(p);
}

cw_anim cw_retarget_clip(cw_anim clip, cw_pose *src, const cw_bone_map *sm, cw_pose *dst, const cw_bone_map *dm,
                         const char *dst_model_path, const cw_retarget_opts *opts) {
  anim_rec *a = cw__anim(clip);
  if (!a || !src || !dst) { cw_error("cw_retarget_clip: needs a clip and two poses"); return (cw_anim){ 0 }; }
  cw_retarget_opts o = opts ? *opts : CW_RETARGET_OPTS_DEFAULT;
  cw_retarget *r = cw_retarget_new(src, sm, dst, dm, &o);
  if (!r) return (cw_anim){ 0 };
  float fps = o.fps > 0 ? o.fps : 30;
  int nt = a->tracks.count;
  int *bone_of = cw_alloc(sizeof(int) * (size_t)(nt ? nt : 1), CW_MEM_ANIM);
  if (!match_tracks(a, src, bone_of)) {
    cw_error("cw_retarget_clip: none of the bone tracks of '%s' name a bone of the source skeleton", a->name);
    cw_free(bone_of);
    cw_retarget_free(r);
    return (cw_anim){ 0 };
  }
  cw_anim out = cw_anim_new(a->name, a->length, a->loop);
  cw_anim_set_stored(out, false);
  const char *model = dst_model_path ? dst_model_path : "";
  /* a rotation track per mapped target bone, a position track for the hips */
  int dtrack[CW_HB_COUNT], hips_track = -1;
  char path[512];
  for (int s = 0; s < CW_HB_COUNT; s++) {
    dtrack[s] = -1;
    int sb = sm->bone[s], db = dm->bone[s];
    if (sb < 0 || db < 0 || sb >= src->count || db >= dst->count || !dst->name[db]) continue;
    snprintf(path, sizeof path, "%s@%s:quaternion", model, dst->name[db]);
    dtrack[s] = cw_anim_add_value_track(out, path, CW_INTERP_LINEAR);
    if (s == CW_HB_HIPS) {
      snprintf(path, sizeof path, "%s@%s:position", model, dst->name[db]);
      hips_track = cw_anim_add_value_track(out, path, CW_INTERP_LINEAR);
    }
  }
  int frames = (int)(a->length * fps + 0.5f);
  if (frames < 1) frames = 1;
  int nk = frames + 1;
  float *times = cw_alloc(sizeof(float) * (size_t)nk, CW_MEM_ANIM);
  for (int f = 0; f < nk; f++) times[f] = f == frames ? a->length : (float)f / fps;
  /* planted feet: where the feet go over the whole clip, then the slide taken out of each stance */
  t3_vec3 *feet[2] = { NULL, NULL };
  if (o.plant_feet && o.foot_ik) {
    for (int side = 0; side < 2; side++) feet[side] = cw_alloc(sizeof(t3_vec3) * (size_t)nk, CW_MEM_ANIM);
    for (int f = 0; f < nk; f++) {
      pose_at(a, bone_of, src, times[f]);
      for (int side = 0; side < 2; side++) feet[side][f] = cw_retarget_foot_target(r, src, side);
    }
    cw_foot_plant_opts po = CW_FOOT_PLANT_OPTS_DEFAULT;
    po.cyclic = o.cyclic;
    cw_foot_plant(feet[0], feet[1], times, nk, a->length, &po);
  }
  for (int f = 0; f < nk; f++) {
    pose_at(a, bone_of, src, times[f]);
    if (feet[0]) cw_retarget_apply_feet(r, src, dst, (t3_vec3[2]){ feet[0][f], feet[1][f] });
    else cw_retarget_apply(r, src, dst);
    for (int s = 0; s < CW_HB_COUNT; s++) {
      if (dtrack[s] < 0) continue;
      int db = dm->bone[s];
      cw_value q = { CW_T_QUAT };
      q.v[0] = dst->rot[db].x; q.v[1] = dst->rot[db].y; q.v[2] = dst->rot[db].z; q.v[3] = dst->rot[db].w;
      cw_anim_add_key(out, dtrack[s], times[f], q);
      if (s == CW_HB_HIPS) cw_anim_add_key(out, hips_track, times[f], cw_val_vec3(dst->pos[db].x, dst->pos[db].y, dst->pos[db].z));
    }
  }
  /* event keys carry over (footsteps, hit windows) */
  for (int i = 0; i < nt; i++) {
    const anim_track *t = &a->tracks.data[i];
    if (t->kind != CW_TRACK_EVENT) continue;
    for (int k = 0; k < t->mkeys.count; k++) cw_anim_add_event(out, t->times.data[k], t->mkeys.data[k].name);
  }
  /* both poses back in their rest pose */
  pose_rest(src);
  pose_rest(dst);
  cw_pose_update_model(src);
  cw_pose_update_model(dst);
  cw_free(feet[0]);
  cw_free(feet[1]);
  cw_free(times);
  cw_free(bone_of);
  cw_retarget_free(r);
  return out;
}

t3_vec3 cw_anim_ground_velocity(cw_anim clip, cw_pose *pose, int left, int right, bool cyclic) {
  anim_rec *a = cw__anim(clip);
  t3_vec3 none = { 0, 0, 0 };
  if (!a || !pose || left < 0 || right < 0 || left >= pose->count || right >= pose->count) return none;
  int nt = a->tracks.count;
  int *bone_of = cw_alloc(sizeof(int) * (size_t)(nt ? nt : 1), CW_MEM_ANIM);
  if (!match_tracks(a, pose, bone_of)) { cw_free(bone_of); return none; }
  /* at the clip's own keys where it has them in a row, else 120 steps: enough for any walk */
  enum { STEPS = 120 };
  static float times[STEPS + 1];
  t3_vec3 *feet[2] = { cw_alloc(sizeof(t3_vec3) * (STEPS + 1), CW_MEM_ANIM), cw_alloc(sizeof(t3_vec3) * (STEPS + 1), CW_MEM_ANIM) };
  for (int k = 0; k <= STEPS; k++) {
    times[k] = a->length * (float)k / STEPS;
    pose_at(a, bone_of, pose, times[k]);
    feet[0][k] = cw_pose_model_position(pose, left);
    feet[1][k] = cw_pose_model_position(pose, right);
  }
  cw_foot_plant_opts po = CW_FOOT_PLANT_OPTS_DEFAULT;
  po.adjust = false;
  po.cyclic = cyclic;
  po.height = 0.05f; /* well inside a stance: the frames easing in and out of one do not count */
  t3_vec3 v = cw_foot_plant(feet[0], feet[1], times, STEPS + 1, a->length, &po);
  pose_rest(pose);
  cw_pose_update_model(pose);
  cw_free(feet[0]);
  cw_free(feet[1]);
  cw_free(bone_of);
  return v;
}

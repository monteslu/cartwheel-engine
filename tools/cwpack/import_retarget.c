/* Retargeting at import (cartwheel/retarget.h): clips of a source glTF are
 * baked onto the skeleton of the model being imported and written into it
 * as ordinary glTF animations, so the game loads them like its own.
 *
 * A model's .cwimport sidecar asks for it:
 *   retarget.source = models/robot.glb     the model that has the clips (a path from the project root)
 *   retarget.clips = walk=Walking attack=Punch
 *                                           which clips, each `new name=source name` or just a name;
 *                                           left out: every clip of the source, under its own name
 *   retarget.fps = 30                       keys per second in the baked clips
 *   retarget.foot_ik = true                 feet where the source's are (planted feet stay planted)
 *   retarget.plant = walk run               clips (by their new names) whose planted feet get their slide taken
 *                                           out: a foot on the ground travels at one steady speed (cw_foot_plant);
 *                                           these clips are taken to repeat (walk and run cycles)
 *   retarget.hand_ik = false                hands where the source's are, relative to the chest
 *   retarget.align_rest = true              line the spine and arms up where the rest poses differ (T-pose, A-pose)
 *   retarget.align_legs = false             the legs too (off: a source that stands wide does not bow the knees)
 *   retarget.hips_scale = 0                 hips travel per unit of the source's; 0: the leg length ratio
 *   retarget.map.<slot> = <bone>            this model's bone for a humanoid slot (left_hand, hips, ...),
 *                                           where the names do not say; `none` clears a slot
 *   retarget.source_map.<slot> = <bone>     the same for the source model
 * Bone maps come from the bone names first (cw_bone_map_auto). Bone names here are the glTF node names;
 * the characters three.js drops from names (. : / [ ] and spaces) may be written or left out.
 *
 * Both skeletons are read from the glTF JSON: every node of the file is a
 * bone of a free cw_pose (parents first), so model space is the scene's. */
#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "cwpack.h"
#include "gltf_doc.h"
#include "cartwheel/retarget.h"

typedef struct {
  cw_pose pose;
  int *node_of;  /* pose bone -> glTF node index */
  int *bone_of;  /* glTF node index -> pose bone, -1 */
  char **names;  /* sanitized names (owned) */
  bool *joint;   /* a joint of a skin: only joints get slots from their names (a mesh node often has a bone's name) */
  int nodes;
} skel;

/* three.js PropertyBinding.sanitizeNodeName: spaces become _, reserved characters go */
static char *sanitize(const char *s) {
  size_t n = strlen(s);
  char *o = cw_alloc(n + 1, CW_MEM_GENERAL);
  size_t k = 0;
  for (size_t i = 0; i < n; i++) {
    char c = s[i];
    if (c == ' ' || c == '\t') o[k++] = '_';
    else if (c == '[' || c == ']' || c == '.' || c == ':' || c == '/' || c == '\\') continue;
    else o[k++] = c;
  }
  o[k] = 0;
  return o;
}

static void node_trs(const cwj *n, t3_vec3 *p, t3_quat *q, t3_vec3 *s) {
  *p = (t3_vec3){ 0, 0, 0 };
  *q = (t3_quat){ 0, 0, 0, 1 };
  *s = (t3_vec3){ 1, 1, 1 };
  cwj *m = cwj_get(n, "matrix"), *t = cwj_get(n, "translation"), *r = cwj_get(n, "rotation"), *sc = cwj_get(n, "scale");
  if (m && m->count == 16) {
    float e[16];
    for (int i = 0; i < 16; i++) e[i] = (float)cwj_num_or(m->items[i], 0);
    *p = (t3_vec3){ e[12], e[13], e[14] };
    float sx = sqrtf(e[0] * e[0] + e[1] * e[1] + e[2] * e[2]), sy = sqrtf(e[4] * e[4] + e[5] * e[5] + e[6] * e[6]),
          sz = sqrtf(e[8] * e[8] + e[9] * e[9] + e[10] * e[10]);
    *s = (t3_vec3){ sx, sy, sz };
    if (sx > 0 && sy > 0 && sz > 0) {
      float m00 = e[0] / sx, m10 = e[1] / sx, m20 = e[2] / sx, m01 = e[4] / sy, m11 = e[5] / sy, m21 = e[6] / sy,
            m02 = e[8] / sz, m12 = e[9] / sz, m22 = e[10] / sz, tr = m00 + m11 + m22;
      if (tr > 0) { float k = 0.5f / sqrtf(tr + 1); *q = (t3_quat){ (m21 - m12) * k, (m02 - m20) * k, (m10 - m01) * k, 0.25f / k }; }
      else if (m00 > m11 && m00 > m22) { float k = 2 * sqrtf(1 + m00 - m11 - m22); *q = (t3_quat){ 0.25f * k, (m01 + m10) / k, (m02 + m20) / k, (m21 - m12) / k }; }
      else if (m11 > m22) { float k = 2 * sqrtf(1 + m11 - m00 - m22); *q = (t3_quat){ (m01 + m10) / k, 0.25f * k, (m12 + m21) / k, (m02 - m20) / k }; }
      else { float k = 2 * sqrtf(1 + m22 - m00 - m11); *q = (t3_quat){ (m02 + m20) / k, (m12 + m21) / k, 0.25f * k, (m10 - m01) / k }; }
    }
    return;
  }
  if (t && t->count == 3) *p = (t3_vec3){ (float)cwj_num_or(t->items[0], 0), (float)cwj_num_or(t->items[1], 0), (float)cwj_num_or(t->items[2], 0) };
  if (r && r->count == 4) *q = (t3_quat){ (float)cwj_num_or(r->items[0], 0), (float)cwj_num_or(r->items[1], 0), (float)cwj_num_or(r->items[2], 0), (float)cwj_num_or(r->items[3], 1) };
  if (sc && sc->count == 3) *s = (t3_vec3){ (float)cwj_num_or(sc->items[0], 1), (float)cwj_num_or(sc->items[1], 1), (float)cwj_num_or(sc->items[2], 1) };
}

static void skel_free(skel *k) {
  for (int i = 0; i < k->pose.count; i++) cw_free(k->names[i]);
  cw_free(k->names);
  cw_free(k->joint);
  cw_free(k->node_of);
  cw_free(k->bone_of);
  cw_pose_free(&k->pose);
}

static void skel_visit(skel *k, cwj *nodes, int node, int parent, int *at) {
  if (node < 0 || node >= k->nodes || k->bone_of[node] >= 0) return;
  int b = (*at)++;
  cwj *n = nodes->items[node];
  k->bone_of[node] = b;
  k->node_of[b] = node;
  k->pose.parent[b] = parent;
  const char *name = cwj_string(n, "name");
  k->names[b] = sanitize(name ? name : "");
  k->pose.name[b] = k->joint[node] ? k->names[b] : NULL;
  node_trs(n, &k->pose.pos[b], &k->pose.rot[b], &k->pose.scl[b]);
  cwj *ch = cwj_get(n, "children");
  for (int i = 0; ch && i < ch->count; i++) skel_visit(k, nodes, (int)cwj_num_or(ch->items[i], -1), b, at);
}

static bool skel_build(skel *k, cw_gltf_doc *d) {
  memset(k, 0, sizeof *k);
  cwj *nodes = cwj_get(d->root, "nodes");
  if (!nodes || !nodes->count) return false;
  k->nodes = nodes->count;
  cw_pose_init(&k->pose, k->nodes);
  k->node_of = cw_calloc((size_t)k->nodes, sizeof(int), CW_MEM_GENERAL);
  k->bone_of = cw_calloc((size_t)k->nodes, sizeof(int), CW_MEM_GENERAL);
  k->names = cw_calloc((size_t)k->nodes, sizeof(char *), CW_MEM_GENERAL);
  k->joint = cw_calloc((size_t)k->nodes, 1, CW_MEM_GENERAL);
  for (int i = 0; i < k->nodes; i++) k->bone_of[i] = -1;
  cwj *skins = cwj_get(d->root, "skins");
  bool any = false;
  for (int s = 0; skins && s < skins->count; s++) {
    cwj *js = cwj_get(skins->items[s], "joints");
    for (int j = 0; js && j < js->count; j++) {
      int x = (int)cwj_num_or(js->items[j], -1);
      if (x >= 0 && x < k->nodes) { k->joint[x] = true; any = true; }
    }
  }
  if (!any) memset(k->joint, 1, (size_t)k->nodes); /* no skin: every node may be a bone */
  /* roots: nodes no node lists as a child, in index order */
  bool *is_child = cw_calloc((size_t)k->nodes, 1, CW_MEM_GENERAL);
  for (int i = 0; i < k->nodes; i++) {
    cwj *ch = cwj_get(nodes->items[i], "children");
    for (int c = 0; ch && c < ch->count; c++) {
      int x = (int)cwj_num_or(ch->items[c], -1);
      if (x >= 0 && x < k->nodes) is_child[x] = true;
    }
  }
  int at = 0;
  for (int i = 0; i < k->nodes; i++)
    if (!is_child[i]) skel_visit(k, nodes, i, -1, &at);
  cw_free(is_child);
  if (at != k->nodes) { /* a cycle left nodes unreached: not a valid glTF */
    skel_free(k);
    return false;
  }
  cw_pose_set_rest(&k->pose);
  cw_pose_update_model(&k->pose);
  return true;
}

/* ── reading a clip ─────────────────────────────────────────────────── */

typedef struct {
  int bone, path; /* 0 translation, 1 rotation, 2 scale */
  int interp;     /* 0 LINEAR, 1 STEP, 2 CUBICSPLINE */
  int count, nc;
  float *times, *values; /* values: nc per key (the value of a cubic spline key, without its tangents) */
} chan;

static float *read_floats(cw_gltf_doc *d, int ai, int *count, int *ncomp) {
  cwj *a = cwj_at(cwj_get(d->root, "accessors"), ai);
  if (!a || cwj_get(a, "sparse")) return NULL;
  int bv = (int)cwj_int(a, "bufferView", -1), ct = (int)cwj_int(a, "componentType", 0);
  int es = cw_gltf_accessor_elem_size(a), cs = ct == 5120 || ct == 5121 ? 1 : ct == 5122 || ct == 5123 ? 2 : 4;
  if (bv < 0 || bv >= d->view_count || !d->views[bv].data || !es) return NULL;
  cwj *view = cwj_at(cwj_get(d->root, "bufferViews"), bv);
  size_t stride = (size_t)cwj_int(view, "byteStride", 0), off = (size_t)cwj_int(a, "byteOffset", 0);
  if (!stride) stride = (size_t)es;
  int n = (int)cwj_int(a, "count", 0), nc = es / cs;
  if (n <= 0 || off + (size_t)(n - 1) * stride + (size_t)es > d->views[bv].len) return NULL;
  float *out = cw_alloc(sizeof(float) * (size_t)n * (size_t)nc, CW_MEM_GENERAL);
  const uint8_t *p = d->views[bv].data + off;
  for (int i = 0; i < n; i++)
    for (int c = 0; c < nc; c++) {
      const uint8_t *q = p + (size_t)i * stride + (size_t)c * (size_t)cs;
      float v;
      switch (ct) { /* integers are normalized (glTF animation outputs) */
      case 5120: v = (float)*(const int8_t *)q / 127.0f; if (v < -1) v = -1; break;
      case 5121: v = (float)*q / 255.0f; break;
      case 5122: { int16_t x; memcpy(&x, q, 2); v = (float)x / 32767.0f; if (v < -1) v = -1; break; }
      case 5123: { uint16_t x; memcpy(&x, q, 2); v = (float)x / 65535.0f; break; }
      default: memcpy(&v, q, 4); break;
      }
      out[i * nc + c] = v;
    }
  *count = n;
  *ncomp = nc;
  return out;
}

static void chan_sample(const chan *c, float t, float out[4]) {
  int n = c->count, nc = c->nc;
  if (n == 1 || t <= c->times[0]) { memcpy(out, c->values, sizeof(float) * (size_t)nc); return; }
  if (t >= c->times[n - 1]) { memcpy(out, c->values + (size_t)(n - 1) * (size_t)nc, sizeof(float) * (size_t)nc); return; }
  int lo = 0, hi = n - 1; /* times[lo] <= t < times[hi] */
  while (hi - lo > 1) {
    int mid = (lo + hi) / 2;
    if (c->times[mid] <= t) lo = mid;
    else hi = mid;
  }
  const float *a = c->values + (size_t)lo * (size_t)nc, *b = c->values + (size_t)hi * (size_t)nc;
  float span = c->times[hi] - c->times[lo], u = span > 0 ? (t - c->times[lo]) / span : 0;
  if (c->interp == 1) u = 0;
  if (c->path == 1) cw_quat_slerp_det(out, a, b, u);
  else for (int k = 0; k < nc; k++) out[k] = a[k] + (b[k] - a[k]) * u;
}

static int add_floats(cw_gltf_doc *d, const float *v, int count, const char *type, int nc, bool minmax) {
  size_t bytes = sizeof(float) * (size_t)count * (size_t)nc;
  uint8_t *b = cw_alloc(bytes ? bytes : 1, CW_MEM_RESOURCE);
  memcpy(b, v, bytes);
  int view = cw_gltf_doc_add_view(d, b, bytes, 0, 0);
  cwj *accs = cwj_get(d->root, "accessors");
  if (!accs) { cwj_set(d->root, "accessors", cwj_arr()); accs = cwj_get(d->root, "accessors"); }
  cwj *a = cwj_obj();
  cwj_set(a, "bufferView", cwj_num(view));
  cwj_set(a, "componentType", cwj_num(5126));
  cwj_set(a, "count", cwj_num(count));
  cwj_set(a, "type", cwj_str(type));
  if (minmax && count) {
    float mn = v[0], mx = v[0];
    for (int i = 1; i < count; i++) { if (v[i] < mn) mn = v[i]; if (v[i] > mx) mx = v[i]; }
    cwj *jmin = cwj_arr(), *jmax = cwj_arr();
    cwj_push(jmin, cwj_numf(mn));
    cwj_push(jmax, cwj_numf(mx));
    cwj_set(a, "min", jmin);
    cwj_set(a, "max", jmax);
  }
  cwj_push(accs, a);
  return accs->count - 1;
}

/* settings "retarget.<prefix><slot> = bone" onto a map made from the names */
static bool apply_map(cw_bone_map *m, const skel *k, const cwp_settings *set, const char *prefix, const char *name) {
  for (int s = 0; s < CW_HB_COUNT; s++) {
    char key[96];
    snprintf(key, sizeof key, "%s%s", prefix, cw_humanoid_bone_name((cw_humanoid_bone)s));
    const char *v = cwp_settings_get(set, key, NULL);
    if (!v) continue;
    if (!strcmp(v, "none")) { m->bone[s] = -1; continue; }
    char *clean = sanitize(v);
    int b = cw_pose_find(&k->pose, clean); /* a joint of that name first, then any node */
    for (int i = 0; b < 0 && i < k->pose.count; i++)
      if (!strcmp(k->names[i], clean)) b = i;
    cw_free(clean);
    if (b < 0) { cwp_report("%s: %s = %s: the model has no node of that name", name, key, v); return false; }
    m->bone[s] = b;
  }
  /* a setting whose slot does not exist is a typo, not a no-op */
  size_t pl = strlen(prefix);
  for (int i = 0; i < set->count; i++) {
    const char *key = set->kv[i].key;
    if (strncmp(key, prefix, pl)) continue;
    if (cw_humanoid_bone_find(key + pl) < 0) { cwp_report("%s: %s: no humanoid slot is named '%s' (hips, spine, left_upper_arm, right_foot, ...)", name, key, key + pl); return false; }
  }
  return true;
}

bool cwp_retarget_bake(cw_gltf_doc *dst, const char *name, const uint8_t *src_file, size_t src_len, const cwp_settings *set,
                       int *clips_out) {
  char err[256];
  cw_gltf_doc sd;
  *clips_out = 0;
  if (!cw_gltf_doc_open(&sd, src_file, src_len, NULL, NULL, err, sizeof err)) { cwp_report("%s: retarget.source: %s", name, err); return false; }
  if (!cw_gltf_doc_meshopt_decode(&sd, err, sizeof err)) { cwp_report("%s: retarget.source: %s", name, err); cw_gltf_doc_free(&sd); return false; }
  skel sk, dk;
  bool ok = false;
  if (!skel_build(&sk, &sd)) { cwp_report("%s: retarget.source has no nodes", name); cw_gltf_doc_free(&sd); return false; }
  if (!skel_build(&dk, dst)) { cwp_report("%s: retarget: the model has no nodes", name); skel_free(&sk); cw_gltf_doc_free(&sd); return false; }
  cw_bone_map sm, dm;
  cw_bone_map_auto(&sm, &sk.pose);
  cw_bone_map_auto(&dm, &dk.pose);
  cw_retarget *rt = NULL;
  chan *chans = NULL;
  float *times = NULL, *rot = NULL, *hip = NULL;
  if (!apply_map(&sm, &sk, set, "retarget.source_map.", name) || !apply_map(&dm, &dk, set, "retarget.map.", name)) goto done;
  char miss[256];
  if (!cw_bone_map_usable(&sm, miss, sizeof miss)) { cwp_report("%s: retarget: the source's bones for %s are not known from its names: set retarget.source_map.<slot> = <node name>", name, miss); goto done; }
  if (!cw_bone_map_usable(&dm, miss, sizeof miss)) { cwp_report("%s: retarget: this model's bones for %s are not known from its names: set retarget.map.<slot> = <node name>", name, miss); goto done; }
  cw_retarget_opts o = CW_RETARGET_OPTS_DEFAULT;
  o.fps = (float)cwp_settings_num(set, "retarget.fps", 30);
  o.foot_ik = cwp_settings_bool(set, "retarget.foot_ik", true);
  o.hand_ik = cwp_settings_bool(set, "retarget.hand_ik", false);
  o.align_rest = cwp_settings_bool(set, "retarget.align_rest", true);
  o.align_legs = cwp_settings_bool(set, "retarget.align_legs", false);
  o.hips_scale = (float)cwp_settings_num(set, "retarget.hips_scale", 0);
  if (o.fps <= 0 || o.fps > 240) { cwp_report("%s: retarget.fps = %g: use 1 to 240", name, o.fps); goto done; }
  rt = cw_retarget_new(&sk.pose, &sm, &dk.pose, &dm, &o);
  if (!rt) { cwp_report("%s: retarget: the two skeletons could not be paired", name); goto done; }

  cwj *sanims = cwj_get(sd.root, "animations");
  const char *want = cwp_settings_get(set, "retarget.clips", "");
  /* the list: each `new=old` or `name`; empty: every clip */
  char list[1024];
  snprintf(list, sizeof list, "%s", want);
  int nwant = 0;
  char *new_name[64], *old_name[64];
  for (char *tok = strtok(list, " ,\t"); tok && nwant < 64; tok = strtok(NULL, " ,\t")) {
    char *eq = strchr(tok, '=');
    if (eq) { *eq = 0; new_name[nwant] = tok; old_name[nwant] = eq + 1; }
    else new_name[nwant] = old_name[nwant] = tok;
    nwant++;
  }
  int total = nwant ? nwant : (sanims ? sanims->count : 0);
  if (!total) { cwp_report("%s: retarget.source has no animations", name); goto done; }
  cwj *danims = cwj_get(dst->root, "animations");
  if (!danims) { cwj_set(dst->root, "animations", cwj_arr()); danims = cwj_get(dst->root, "animations"); }
  for (int ci = 0; ci < total; ci++) {
    cwj *sa = NULL;
    const char *out_name;
    if (nwant) {
      for (int i = 0; sanims && i < sanims->count; i++) {
        const char *an = cwj_string(sanims->items[i], "name");
        if (an && !strcmp(an, old_name[ci])) { sa = sanims->items[i]; break; }
      }
      if (!sa) { cwp_report("%s: retarget.clips: the source has no animation named '%s'", name, old_name[ci]); goto done; }
      out_name = new_name[ci];
    } else {
      sa = sanims->items[ci];
      out_name = cwj_string(sa, "name");
      if (!out_name) out_name = "clip";
    }
    cwj *sch = cwj_get(sa, "channels"), *ssm = cwj_get(sa, "samplers");
    int nch = sch ? sch->count : 0, used = 0;
    chans = cw_calloc((size_t)(nch ? nch : 1), sizeof *chans, CW_MEM_GENERAL);
    float length = 0;
    for (int i = 0; i < nch; i++) {
      cwj *target = cwj_get(sch->items[i], "target"), *smp = cwj_at(ssm, (int)cwj_int(sch->items[i], "sampler", -1));
      const char *path = cwj_string(target, "path"), *interp = cwj_string(smp, "interpolation");
      int node = (int)cwj_int(target, "node", -1);
      int p = !path ? -1 : !strcmp(path, "translation") ? 0 : !strcmp(path, "rotation") ? 1 : !strcmp(path, "scale") ? 2 : -1;
      if (p < 0 || node < 0 || node >= sk.nodes || !smp) continue; /* morph weights and pointer targets stay with the source */
      chan *c = &chans[used];
      int tn = 0, tc = 0, vn = 0, vc = 0;
      c->times = read_floats(&sd, (int)cwj_int(smp, "input", -1), &tn, &tc);
      float *vals = read_floats(&sd, (int)cwj_int(smp, "output", -1), &vn, &vc);
      c->interp = !interp || !strcmp(interp, "LINEAR") ? 0 : !strcmp(interp, "STEP") ? 1 : 2;
      int per = c->interp == 2 ? 3 : 1;
      if (!c->times || !vals || tc != 1 || vn != tn * per || vc != (p == 1 ? 4 : 3)) {
        cwp_report("%s: retarget: animation '%s' of the source has a channel that cannot be read", name, out_name);
        cw_free(c->times);
        cw_free(vals);
        for (int k = 0; k < used; k++) { cw_free(chans[k].times); cw_free(chans[k].values); }
        goto done;
      }
      if (per == 3) { /* keep each key's value; the spline's tangents are not used (the bake samples densely) */
        for (int k = 0; k < tn; k++) memmove(vals + (size_t)k * (size_t)vc, vals + ((size_t)k * 3 + 1) * (size_t)vc, sizeof(float) * (size_t)vc);
        c->interp = 0;
      }
      c->values = vals;
      c->count = tn;
      c->nc = vc;
      c->bone = sk.bone_of[node];
      c->path = p;
      if (c->times[tn - 1] > length) length = c->times[tn - 1];
      used++;
    }
    int frames = (int)(length * o.fps + 0.5f);
    if (frames < 1) frames = 1;
    int nk = frames + 1, nrot = 0;
    bool plant = false;
    {
      char pl[512];
      snprintf(pl, sizeof pl, "%s", cwp_settings_get(set, "retarget.plant", ""));
      for (char *tok2 = strtok(pl, " ,	"); tok2; tok2 = strtok(NULL, " ,	")) plant |= !strcmp(tok2, out_name);
    }
    for (int s = 0; s < CW_HB_COUNT; s++) nrot += sm.bone[s] >= 0 && dm.bone[s] >= 0;
    times = cw_alloc(sizeof(float) * (size_t)nk, CW_MEM_GENERAL);
    rot = cw_alloc(sizeof(float) * 4 * (size_t)nk * (size_t)(nrot ? nrot : 1), CW_MEM_GENERAL);
    hip = cw_alloc(sizeof(float) * 3 * (size_t)nk, CW_MEM_GENERAL);
    t3_vec3 *feet[2] = { NULL, NULL };
    for (int pass = plant && o.foot_ik ? 0 : 1; pass < 2; pass++)
    for (int f = 0; f < nk; f++) {
      float t = f == frames ? length : (float)f / o.fps;
      times[f] = t;
      if (pass == 0 && !feet[0]) {
        feet[0] = cw_alloc(sizeof(t3_vec3) * (size_t)nk, CW_MEM_GENERAL);
        feet[1] = cw_alloc(sizeof(t3_vec3) * (size_t)nk, CW_MEM_GENERAL);
      }
      for (int i = 0; i < sk.pose.count; i++) {
        sk.pose.pos[i] = sk.pose.rest_pos[i];
        sk.pose.rot[i] = sk.pose.rest_rot[i];
        sk.pose.scl[i] = sk.pose.rest_scl[i];
      }
      for (int i = 0; i < used; i++) {
        float v[4] = { 0, 0, 0, 1 };
        chan_sample(&chans[i], t, v);
        int b = chans[i].bone;
        if (chans[i].path == 0) sk.pose.pos[b] = (t3_vec3){ v[0], v[1], v[2] };
        else if (chans[i].path == 1) sk.pose.rot[b] = (t3_quat){ v[0], v[1], v[2], v[3] };
        else sk.pose.scl[b] = (t3_vec3){ v[0], v[1], v[2] };
      }
      cw_pose_update_model(&sk.pose);
      if (pass == 0) { /* where the feet go over the whole clip; after the last frame, the slide comes out */
        feet[0][f] = cw_retarget_foot_target(rt, &sk.pose, 0);
        feet[1][f] = cw_retarget_foot_target(rt, &sk.pose, 1);
        if (f == nk - 1) {
          cw_foot_plant_opts po = CW_FOOT_PLANT_OPTS_DEFAULT;
          po.cyclic = true;
          cw_foot_plant(feet[0], feet[1], times, nk, length, &po);
        }
        continue;
      }
      if (feet[0]) cw_retarget_apply_feet(rt, &sk.pose, &dk.pose, (t3_vec3[2]){ feet[0][f], feet[1][f] });
      else cw_retarget_apply(rt, &sk.pose, &dk.pose);
      int r = 0;
      for (int s = 0; s < CW_HB_COUNT; s++) {
        if (sm.bone[s] < 0 || dm.bone[s] < 0) continue;
        t3_quat q = dk.pose.rot[dm.bone[s]];
        float *o4 = rot + ((size_t)r * (size_t)nk + (size_t)f) * 4;
        o4[0] = q.x; o4[1] = q.y; o4[2] = q.z; o4[3] = q.w;
        r++;
      }
      t3_vec3 hp = dk.pose.pos[dm.bone[CW_HB_HIPS]];
      hip[f * 3] = hp.x; hip[f * 3 + 1] = hp.y; hip[f * 3 + 2] = hp.z;
    }
    /* the baked animation: one input shared by every sampler */
    cwj *anim = cwj_obj(), *samplers = cwj_arr(), *channels = cwj_arr();
    cwj_set(anim, "name", cwj_str(out_name));
    int input = add_floats(dst, times, nk, "SCALAR", 1, true);
    int r = 0;
    for (int s = 0; s <= CW_HB_COUNT; s++) {
      bool hips_pos = s == CW_HB_COUNT;
      int slot = hips_pos ? CW_HB_HIPS : s;
      if (sm.bone[slot] < 0 || dm.bone[slot] < 0) continue;
      int out = hips_pos ? add_floats(dst, hip, nk, "VEC3", 3, false) : add_floats(dst, rot + (size_t)r * (size_t)nk * 4, nk, "VEC4", 4, false);
      if (!hips_pos) r++;
      cwj *smp = cwj_obj(), *ch = cwj_obj(), *target = cwj_obj();
      cwj_set(smp, "input", cwj_num(input));
      cwj_set(smp, "output", cwj_num(out));
      cwj_set(smp, "interpolation", cwj_str("LINEAR"));
      cwj_push(samplers, smp);
      cwj_set(target, "node", cwj_num(dk.node_of[dm.bone[slot]]));
      cwj_set(target, "path", cwj_str(hips_pos ? "translation" : "rotation"));
      cwj_set(ch, "sampler", cwj_num(samplers->count - 1));
      cwj_set(ch, "target", target);
      cwj_push(channels, ch);
    }
    cwj_set(anim, "samplers", samplers);
    cwj_set(anim, "channels", channels);
    /* a clip of that name already in the model is replaced */
    bool replaced = false;
    for (int i = 0; i < danims->count; i++) {
      const char *an = cwj_string(danims->items[i], "name");
      if (an && !strcmp(an, out_name)) { cwj_free(danims->items[i]); danims->items[i] = anim; replaced = true; break; }
    }
    if (!replaced) cwj_push(danims, anim);
    for (int k = 0; k < used; k++) { cw_free(chans[k].times); cw_free(chans[k].values); }
    cw_free(chans); chans = NULL;
    cw_free(times); times = NULL;
    cw_free(rot); rot = NULL;
    cw_free(hip); hip = NULL;
    cw_free(feet[0]);
    cw_free(feet[1]);
    (*clips_out)++;
  }
  ok = true;
done:
  cw_free(chans);
  cw_free(times);
  cw_free(rot);
  cw_free(hip);
  cw_retarget_free(rt);
  skel_free(&sk);
  skel_free(&dk);
  cw_gltf_doc_free(&sd);
  return ok;
}

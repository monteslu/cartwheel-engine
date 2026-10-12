/* Shared helpers of the 2D and 3D physics nodes. */
#include <stdio.h>
#include <stdlib.h>

#include "phys_nodes_internal.h"

const char *cw__s_body_entered, *cw__s_body_exited, *cw__s_area_entered, *cw__s_area_exited, *cw__s_body_hit, *cw__s_broken;

void cw__phys_signals_init(void) {
  if (cw__s_body_entered) return;
  cw__s_body_entered = cw_intern("body_entered");
  cw__s_body_exited = cw_intern("body_exited");
  cw__s_area_entered = cw_intern("area_entered");
  cw__s_area_exited = cw_intern("area_exited");
  cw__s_body_hit = cw_intern("body_hit");
  cw__s_broken = cw_intern("broken");
}

void cw__psig_deliver(cw_tree *t, cw__psig_list *l) {
  if (!l->count) return;
  bool was = t->processing;
  t->processing = true;
  for (int i = 0; i < l->count; i++) {
    cw__psig *s = &l->data[i];
    if (!cw_node_valid(s->emitter)) continue;
    cw_value args[2] = { { CW_T_NODE }, cw_val_float(s->f) };
    args[0].node = s->arg;
    cw_emit(s->emitter, s->signal, args, s->argc);
  }
  cw_array_clear(l);
  t->processing = was;
  if (!was) {
    cw__tree_apply_deferred(t);
    cw__signals_flush(t);
    cw__tree_apply_deferred(t);
  }
}

int cw__pair_add(cw_map *m, uint64_t other, int delta) {
  if (!m->slot_cap && !m->count) cw_map_init(m, CW_MEM_PHYSICS);
  uint64_t v = 0;
  cw_map_get(m, other, &v);
  int64_t c = (int64_t)v + delta;
  if (c <= 0) {
    cw_map_remove(m, other);
    return 0;
  }
  cw_map_set(m, other, (uint64_t)c);
  return (int)c;
}

int cw__layer_index(const char *const names[CW_PHYS_LAYERS], const char *name) {
  const char *in = cw_intern(name);
  for (int i = 0; i < CW_PHYS_LAYERS; i++)
    if (names[i] == in) return i;
  char *end;
  long k = strtol(name, &end, 10);
  if (end != name && !*end && k >= 0 && k < CW_PHYS_LAYERS) return (int)k;
  cw_error("physics: no layer named '%s' (set layer_names_2d / layer_names_3d in project.cwproj)", name);
  return -1;
}

uint32_t cw__layer_bits(const char *const names[CW_PHYS_LAYERS], const char *list) {
  uint32_t bits = 0;
  const char *p = list ? list : "";
  if (!strcmp(p, "*") || !strcmp(p, "all")) return CW_PHYS_ALL_LAYERS;
  while (*p) {
    while (*p == ' ' || *p == ',' || *p == '|') p++;
    const char *s = p;
    while (*p && *p != ',' && *p != '|') p++;
    const char *e = p;
    while (e > s && e[-1] == ' ') e--;
    if (e > s) {
      char buf[128];
      snprintf(buf, sizeof buf, "%.*s", (int)(e - s), s);
      int k = cw__layer_index(names, buf);
      if (k >= 0) bits |= 1u << k;
    }
  }
  return bits;
}

void cw__layer_bits_text(const char *const names[CW_PHYS_LAYERS], uint32_t bits, char *out, int cap) {
  int n = 0;
  out[0] = 0;
  if (bits == CW_PHYS_ALL_LAYERS) { snprintf(out, (size_t)cap, "all"); return; }
  for (int i = 0; i < CW_PHYS_LAYERS; i++)
    if (bits & (1u << i)) {
      if (names[i]) n += snprintf(out + n, (size_t)(cap > n ? cap - n : 0), "%s%s", n ? "|" : "", names[i]);
      else n += snprintf(out + n, (size_t)(cap > n ? cap - n : 0), "%s%d", n ? "|" : "", i);
    }
}

/* ── debug channel: physics state and a way to push bodies ──────────── */

#include "cartwheel/debug.h"

static void stats_json(cw_reply *r, const char *key, cw_phys_stats s, bool first) {
  cw_reply_printf(r, "%s\"%s\":{\"bodies\":%d,\"awake\":%d,\"shapes\":%d,\"contacts\":%d,\"joints\":%d,\"events\":%d,"
                     "\"steps\":%d,\"hash\":%u}",
                  first ? "" : ",", key, s.bodies, s.awake_bodies, s.shapes, s.contacts, s.joints, s.events, s.steps, s.hash);
}

static void layers_json(cw_reply *r, const char *key, const char *const *names) {
  cw_reply_printf(r, ",\"%s\":[", key);
  for (int i = 0; i < CW_PHYS_LAYERS; i++) {
    if (i) cw_reply_raw(r, ",");
    if (names[i]) cw_reply_str(r, names[i]);
    else cw_reply_raw(r, "null");
  }
  cw_reply_raw(r, "]");
}

static void cmd_phys(int argc, const char **argv, cw_reply *r) {
  (void)argc;
  (void)argv;
  cw_tree *t = cw_current_tree();
  if (!t) { cw_reply_error(r, "no scene tree"); return; }
  cw_reply_raw(r, "{");
  bool first = true;
  if (cw_tree_slot(t, "physics2d")) {
    stats_json(r, "2d", cw_world2d_stats(cw_physics2d_world(t)), first);
    cw_reply_printf(r, ",\"pixels_per_meter\":%g", (double)cw_physics2d_ppm(t));
    first = false;
  }
  if (cw_tree_slot(t, "physics3d")) {
    stats_json(r, "3d", cw_world3d_stats(cw_physics3d_world(t)), first);
    first = false;
  }
  if (first) cw_reply_raw(r, "\"worlds\":0");
  const cw_physics_nodes_config *c = cw_physics_nodes_get_config();
  layers_json(r, "layers2d", c->layers_2d);
  layers_json(r, "layers3d", c->layers_3d);
  cw_reply_raw(r, "}");
}

static void cmd_impulse(int argc, const char **argv, cw_reply *r) {
  if (argc < 4) { cw_reply_error(r, "usage: impulse <rigid body path> x y [z]  (2D: px*kg/s, y down; 3D: N*s)"); return; }
  cw_tree *t = cw_current_tree();
  cw_node n = t ? cw_node_get(cw_tree_root(t), argv[1]) : (cw_node){ 0 };
  if (cw_node_is(n, "RigidBody2D")) cw_rigid_body2d_apply_impulse(n, cw_v2((float)atof(argv[2]), (float)atof(argv[3])));
  else if (cw_node_is(n, "RigidBody3D") && argc > 4)
    cw_rigid_body3d_apply_impulse(n, (t3_vec3){ (float)atof(argv[2]), (float)atof(argv[3]), (float)atof(argv[4]) });
  else { cw_reply_error(r, "'%s' is not a RigidBody2D/3D (or a 3D impulse lacks z)", argv[1]); return; }
  cw_reply_raw(r, "{\"ok\":true}");
}

void cw__phys_register_debug_commands(void) {
  cw_debug_command("phys", "phys: the tree's physics worlds (stats, state hash) and project layer names", cmd_phys);
  cw_debug_command("impulse", "impulse <path> x y [z]: push a RigidBody2D/3D", cmd_impulse);
}

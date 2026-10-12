/* turntable (runtime part): a Turntable node type and a `turntables` debug
 * command, registered through the engine's ordinary registries from the
 * plugin's register_fn (cartwheel/plugin.h). Nothing here is compiled into
 * the engine: the project build adds it to the game (and the native editor
 * loads it as plugins/turntable/turntable_runtime.so).
 *
 * Turntable (a Node3D): turns about `axis` at `speed` radians per second,
 * carrying its children. `radius` is the reach of what sits on it (the
 * editor's gizmo draws and drags it); `tip_speed` = speed * radius. */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cartwheel/debug.h"
#include "cartwheel/plugin.h"
#include "cartwheel/scene.h"
#include "cartwheel/scene_file.h"

typedef struct turntable {
  float speed;  /* radians per second (negative = clockwise seen from +axis) */
  t3_vec3 axis;
  float radius;
  float angle;  /* runtime: radians turned so far */
  int32_t turns; /* runtime: whole turns completed */
} turntable;

static const cw_node_type *T;

static void tt_init(cw_node n, void *data) {
  (void)n;
  turntable *t = data;
  t->speed = 1.0f;
  t->axis = (t3_vec3){ 0, 1, 0 };
  t->radius = 1.0f;
}

static void tt_fixed(cw_node n, void *data, double dt) {
  turntable *t = data;
  t->angle += t->speed * (float)dt;
  t->turns = (int32_t)(fabsf(t->angle) / 6.2831853f);
  float len = sqrtf(t->axis.x * t->axis.x + t->axis.y * t->axis.y + t->axis.z * t->axis.z);
  t3_vec3 a = len > 1e-6f ? (t3_vec3){ t->axis.x / len, t->axis.y / len, t->axis.z / len } : (t3_vec3){ 0, 1, 0 };
  cw_node3d_snapshot(n);
  cw_node3d_set_quaternion(n, t3_quat_from_axis_angle(a, t->angle));
}

static void get_tip(cw_node n, void *data, cw_value *out) {
  (void)n;
  turntable *t = data;
  *out = cw_val_float(t->speed * t->radius);
}

/* #region type */
static const cw_prop tt_props[] = {
  CW_PROP_FIELD_RANGE(turntable, speed, CW_T_FLOAT, -20, 20, "radians per second about axis (negative: the other way)"),
  CW_PROP_FIELD(turntable, axis, CW_T_VEC3, CW_PROP_DEFAULT, "the axis it turns about (local; normalized when used)"),
  CW_PROP_FIELD_RANGE(turntable, radius, CW_T_FLOAT, 0, 100, "reach of what sits on it (the editor gizmo drags it)"),
  CW_PROP_FIELD(turntable, angle, CW_T_FLOAT, CW_PROP_EDITOR | CW_PROP_READONLY, "radians turned since it entered (runtime)"),
  CW_PROP_FIELD(turntable, turns, CW_T_INT, CW_PROP_EDITOR | CW_PROP_READONLY, "whole turns completed (runtime)"),
  CW_PROP_ACCESSOR("tip_speed", CW_T_FLOAT, CW_PROP_EDITOR | CW_PROP_READONLY, get_tip, NULL, "speed * radius (units per second at the rim)"),
  CW_PROP_END
};

static cw_node_type turntable_type = {
  .name = "Turntable", .parent = "Node3D", .data_size = sizeof(turntable), .props = tt_props,
  .doc = "Turns about an axis at a set speed, carrying its children (plugin turntable).",
  .init = tt_init, .fixed_update = tt_fixed,
};
/* #endregion */

static bool m_reverse(cw_node self, const cw_value *args, int argc, cw_value *ret, void *user) {
  (void)args, (void)argc, (void)user;
  turntable *t = cw_node_data_t(self, T);
  t->speed = -t->speed;
  *ret = cw_val_float(t->speed);
  return true;
}

/* ── `turntables [speed <v>]`: every Turntable of the running scene ───── */

typedef struct { cw_reply *r; int count; bool set; float speed; } walk_ctx;

static bool visit(cw_node n, void *user) {
  walk_ctx *w = user;
  turntable *t = cw_node_data_t(n, T);
  if (!t) return true;
  if (w->set) t->speed = w->speed;
  char path[256];
  cw_node_path(n, (cw_node){ 0 }, path, sizeof path);
  cw_reply_printf(w->r, "%s{\"path\":", w->count ? "," : "");
  cw_reply_str(w->r, path);
  cw_reply_printf(w->r, ",\"uid\":%llu,\"speed\":%.6g,\"radius\":%.6g,\"angle\":%.6g,\"turns\":%d,\"tip_speed\":%.6g}",
                  (unsigned long long)cw_node_uid(n), t->speed, t->radius, t->angle, t->turns, t->speed * t->radius);
  w->count++;
  return true;
}

static void cmd_turntables(int argc, const char **argv, cw_reply *r) {
  walk_ctx w = { r, 0, false, 0 };
  if (argc >= 3 && !strcmp(argv[1], "speed")) {
    char *end;
    w.speed = strtof(argv[2], &end);
    if (end == argv[2] || *end) {
      cw_reply_error(r, "turntables speed <radians per second>: '%s' is not a number (e.g. turntables speed 2.5)", argv[2]);
      return;
    }
    w.set = true;
  } else if (argc > 1) {
    cw_reply_error(r, "usage: turntables [speed <radians per second>]");
    return;
  }
  cw_tree *tree = cw_current_tree();
  if (!tree) {
    cw_reply_error(r, "no scene tree is running");
    return;
  }
  cw_reply_raw(r, "{\"turntables\":[");
  cw_node_walk(cw_tree_root(tree), visit, &w);
  cw_reply_printf(r, "],\"count\":%d}", w.count);
}

/* #region register */
static void reg(void) {
  cw_node_type_register(&turntable_type);
  T = cw_node_type_find("Turntable");
  cw_node_type_add_method("Turntable", "reverse", "", m_reverse, NULL, "turn the other way (speed = -speed); returns the new speed");
  cw_debug_command("turntables", "turntables [speed <v>]: every Turntable (path, speed, radius, angle, turns); speed sets them all",
                   cmd_turntables);
}

CW_PLUGIN(turntable, .version = "1.0.0", .register_fn = reg);
/* #endregion */

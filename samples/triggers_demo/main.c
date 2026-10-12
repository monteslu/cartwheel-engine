/* triggers_demo: data-driven "when X then Y" rules (Trigger nodes,
 * cartwheel/triggers.h) with no game code behind them.
 *
 * A ball (group player) and a crate (group crates) fall through a Goal area
 * and the ball then reaches an Exit area. Every reaction is a Trigger in
 * assets/main.cwscene:
 *   Goal body_entered [player]  SET Lamp.color green
 *   Goal body_entered           CALL Counter.add(1)          (ball and crate: +2)
 *   Goal body_entered [player]  EMIT Counter bump -> Counter's own trigger
 *                               CALLs add(10)                (a chained rule)
 *   Goal body_entered [player]  SPAWN spark.cwscene under Lamp, once
 *   Goal body_entered [crates]  FREE the Crate
 *   Goal body_entered           (disabled) SET Lamp.visible false: never runs
 *   Exit body_entered [player]  CHANGE_SCENE won.cwscene
 * The only C here is the Counter node type (a signal and a method a rule can
 * use) and the probes. CW_TRIGGERS_OFF=1 disables every trigger (the gate's
 * control). */
#include <stdlib.h>
#include <string.h>

#include "cartwheel/debug.h"
#include "cartwheel/scene_file.h"
#include "cartwheel/triggers.h"

/* #region counter */
typedef struct { int32_t hits; } counter;

static const char *const counter_signals[] = { "bump", NULL };
static const cw_prop counter_props[] = {
  CW_PROP_FIELD(counter, hits, CW_T_INT, CW_PROP_DEFAULT, "Sum of every add()."),
  CW_PROP_END,
};
static cw_node_type counter_type = { .name = "Counter", .parent = "Node", .data_size = sizeof(counter), .props = counter_props,
                                     .signals = counter_signals, .doc = "Adds what add(n) gets; emits bump when told to." };

static bool counter_add(cw_node self, const cw_value *args, int argc, cw_value *ret, void *user) {
  (void)user;
  if (argc != 1 || args[0].type != CW_T_INT) return false;
  counter *c = cw_node_data(self, "Counter");
  c->hits += (int32_t)args[0].i;
  if (ret) *ret = cw_val_int(c->hits);
  return true;
}

/* Every node type and method this game defines (the editor loads them). */
void cw_game_register_types(void);
void cw_game_register_types(void) {
  cw_node_type_register(&counter_type);
  cw_node_type_add_method("Counter", "add", "int", counter_add, NULL, "add n to hits");
}
/* #endregion */

static struct {
  int32_t tick, hits, lamp_g, lamp_visible, spark, crate, won, tick_lamp, tick_won, disabled;
  int32_t fired[7];
} P;
static const char *const RULES[] = { "Goal/LightLamp", "Goal/Count", "Goal/Bump", "Goal/Spark", "Goal/DropCrate", "Goal/Never",
                                     "Counter/OnBump" };

static bool disable(cw_node n, void *u) {
  if (cw_node_is(n, "Trigger")) {
    cw_node_set(n, "enabled", cw_val_bool(false));
    (*(int32_t *)u)++;
  }
  return true;
}

static void fixed(void *u, double dt) {
  (void)u, (void)dt;
  cw_tree *t = cw_scene_app_tree();
  cw_node level = cw_node_get(cw_tree_root(t), "/root/Level");
  if (P.tick == 0 && getenv("CW_TRIGGERS_OFF") && cw_node_valid(level)) cw_node_walk(level, disable, &P.disabled);
  P.tick++;
  const char *scene = cw_scene_app_current_scene();
  if (scene && !strcmp(scene, "won.cwscene")) {
    if (!P.won) P.tick_won = P.tick;
    P.won = 1;
    return; /* the main scene's probes keep their last values */
  }
  if (!cw_node_valid(level)) return;
  cw_value v;
  cw_node counter = cw_node_get(level, "Counter"), lamp = cw_node_get(level, "Lamp");
  if (cw_node_get_prop(counter, "hits", &v)) P.hits = (int32_t)v.i;
  if (cw_node_get_prop(lamp, "color", &v)) {
    int32_t g = (int32_t)(v.v[1] * 100 + 0.5f);
    if (g != P.lamp_g && P.tick > 1 && !P.tick_lamp) P.tick_lamp = P.tick;
    P.lamp_g = g;
  }
  if (cw_node_get_prop(lamp, "visible", &v)) P.lamp_visible = v.b;
  P.spark = cw_node_valid(cw_node_get(level, "Lamp/Spark"));
  P.crate = cw_node_valid(cw_node_get(level, "Crate"));
  for (int i = 0; i < 7; i++)
    if (cw_node_get_prop(cw_node_get(level, RULES[i]), "fired", &v)) P.fired[i] = (int32_t)v.i;
}

static void init(void *u) {
  (void)u;
  cw_debug_i32("tick", &P.tick);
  cw_debug_i32("hits", &P.hits);
  cw_debug_i32("lamp_g", &P.lamp_g);
  cw_debug_i32("lamp_visible", &P.lamp_visible);
  cw_debug_i32("spark", &P.spark);
  cw_debug_i32("crate", &P.crate);
  cw_debug_i32("won", &P.won);
  cw_debug_i32("tick_lamp", &P.tick_lamp);
  cw_debug_i32("tick_won", &P.tick_won);
  cw_debug_i32("disabled", &P.disabled);
  cw_debug_field("fired", P.fired, CW_DBG_I32, 7);
}

cw_app_desc cw_main(int argc, char **argv) {
  (void)argc;
  (void)argv;
  cw_scene_register_builtin_types();
  cw_game_register_types();
  static cw_app_desc base = { .title = "triggers_demo", .width = 640, .height = 360, .debug = true, .init = init, .fixed_update = fixed };
  return cw_scene_app("main.cwscene", &base);
}

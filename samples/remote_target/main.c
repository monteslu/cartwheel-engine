/* remote_target: the game tests/remote.mjs launches through
 * cartwheel/remote.h (play-in-editor, M4.3) on every transport.
 *
 * A Mover node type moves right at `speed` pixels per second in its fixed
 * tick and counts its ticks, so an edit made while paused shows up as an
 * exact position after a known number of steps. It has two methods (boost,
 * teleport) for `call`. The world layer is the ground and the mover, the UI
 * layer is a Label (Hud), and a debug-draw pass (OVERLAY 1000) outlines the
 * mover, so each layer of a layer screenshot has something only it draws.
 * data/config.txt is loaded as a text resource; when it is reloaded the Hud
 * shows its new text (`reload` changes game state). The action "nudge"
 * (space / pad south) adds 5 to every mover's speed, for `record`. */
#include <math.h>
#include <string.h>

#include "cartwheel/debug.h"
#include "cartwheel/input.h"
#include "cartwheel/pipeline.h"
#include "cartwheel/render2d.h"
#include "cartwheel/resource.h"
#include "cartwheel/scene_file.h"

typedef struct { float speed; int32_t ticks; } mover;

static const cw_prop mover_props[] = {
  CW_PROP_FIELD(mover, speed, CW_T_FLOAT, CW_PROP_DEFAULT, "pixels per second along +x"),
  { .name = "ticks", .type = CW_T_INT, .flags = CW_PROP_READONLY | CW_PROP_EDITOR, .offset = offsetof(mover, ticks),
    .size = sizeof(int32_t), .doc = "fixed ticks this mover has run" },
  CW_PROP_END,
};

static struct {
  cw_action nudge;
  cw_res config;
  uint32_t config_version;
} G;

static void mover_fixed(cw_node n, void *data, double dt) {
  mover *m = data;
  if (cw_action_pressed(0, G.nudge)) m->speed += 5;
  cw_vec2 p = cw_node2d_position(n);
  p.x += m->speed * (float)dt;
  cw_node2d_set_position(n, p);
  m->ticks++;
}

static cw_node_type mover_type = { .name = "Mover", .parent = "Node2D", .data_size = sizeof(mover), .props = mover_props,
                                   .doc = "Moves along +x at `speed` px/s each fixed tick.", .fixed_update = mover_fixed };

/* #region boost */
static bool m_boost(cw_node self, const cw_value *args, int argc, cw_value *ret, void *user) {
  (void)user;
  if (argc != 1 || args[0].type != CW_T_FLOAT) return false;
  mover *m = cw_node_data(self, "Mover");
  m->speed += (float)args[0].f;
  *ret = cw_val_float(m->speed);
  return true;
}
/* #endregion */

static bool m_teleport(cw_node self, const cw_value *args, int argc, cw_value *ret, void *user) {
  (void)user;
  (void)ret;
  if (argc != 1 || args[0].type != CW_T_VEC2) return false;
  cw_node2d_set_position(self, (cw_vec2){ args[0].v[0], args[0].v[1] });
  return true;
}

/* debug draw: a magenta outline around every mover */
static bool outline(cw_node n, void *user) {
  (void)user;
  if (cw_node_is(n, "Mover")) {
    cw_vec2 p = cw_node2d_global_position(n);
    cw_r2d_rect_lines(cw_rect_make(p.x - 12, p.y - 12, 24, 24), 2, (cw_color){ 1, 0, 1, 1 });
  }
  return true;
}
/* #region debug_pass_exec */
static void debug_draw(cw_pass_ctx *ctx, void *user) {
  (void)user;
  cw_r2d_begin(&(cw_r2d_pass_desc){ .clear = false });
  cw_node_walk(cw_tree_root(ctx->tree), outline, NULL);
  cw_r2d_end();
}
/* #endregion */

/* Every node type and method this game defines (the editor loads them). */
/* #region register_types */
void cw_game_register_types(void);
void cw_game_register_types(void) {
  cw_node_type_register(&mover_type);
  cw_node_type_add_method("Mover", "boost", "float", m_boost, NULL, "add to speed; returns the new speed");
  cw_node_type_add_method("Mover", "teleport", "vec2", m_teleport, NULL, "jump to a position");
}
/* #endregion */

static void init(void *u) {
  (void)u;
  cw_game_register_types();
  G.nudge = cw_action_add("nudge", 0.5f);
  cw_action_bind_text(G.nudge, "key:space, pad:south");
  G.config = cw_res_load("data/config.txt", CW_RES_TEXT);
  G.config_version = cw_res_version(G.config);
  /* the type exists now: the main scene can be instanced */
  cw_scene_app_change_scene("main.cwscene", false);
  cw_tree *t = cw_scene_app_tree();
  /* #region debug_pass_add */
  cw_pipeline_add_pass(cw_pipeline_of(t), &(cw_pass_desc){ .name = "debug_draw", .phase = CW_PHASE_OVERLAY, .order = 1000,
                                                          .execute = debug_draw });
  /* #endregion */
}

static void update(void *u, double dt) {
  (void)u;
  (void)dt;
  if (cw_res_version(G.config) == G.config_version) return;
  G.config_version = cw_res_version(G.config);
  const cw_blob *b = cw_res_get(G.config);
  cw_node hud = cw_node_get(cw_tree_root(cw_scene_app_tree()), "Level/Hud");
  if (b && cw_node_valid(hud)) {
    char text[256];
    size_t n = b->len < sizeof text - 1 ? b->len : sizeof text - 1;
    memcpy(text, b->data, n);
    while (n && (text[n - 1] == '\n' || text[n - 1] == '\r')) n--;
    text[n] = 0;
    cw_node_set(hud, "text", cw_val_string(text));
  }
}

cw_app_desc cw_main(int argc, char **argv) {
  (void)argc;
  (void)argv;
  static cw_app_desc base = { .title = "remote_target", .width = 320, .height = 180, .debug = true, .init = init,
                              .update = update };
  return cw_scene_app(NULL, &base);
}

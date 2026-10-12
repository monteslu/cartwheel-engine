/* Agent Demo: a box that spins. Its colour and speed are compile-time
 * constants, so changing them is a C edit + rebuild + hot reload: the loop
 * the editor's agent panel drives (tests/editor-agent.mjs). */
#include "cartwheel/scene_file.h"

#define SPIN_SPEED 0.02f
#define BOX_COLOR 0.787f, 0.214f, 0.033f

typedef struct { float angle, speed; int32_t ticks; } spinner;

static const cw_prop spinner_props[] = {
  { .name = "angle", .type = CW_T_FLOAT, .flags = CW_PROP_EDITOR | CW_PROP_READONLY, .offset = offsetof(spinner, angle),
    .size = sizeof(float), .doc = "Turned so far (radians)." },
  { .name = "speed", .type = CW_T_FLOAT, .flags = CW_PROP_EDITOR | CW_PROP_READONLY, .offset = offsetof(spinner, speed),
    .size = sizeof(float), .doc = "Radians per tick the running code turns (SPIN_SPEED)." },
  { .name = "ticks", .type = CW_T_INT, .flags = CW_PROP_EDITOR | CW_PROP_READONLY, .offset = offsetof(spinner, ticks),
    .size = sizeof(int32_t), .doc = "Ticks run." },
  CW_PROP_END,
};

static void spin_fixed(cw_node n, void *data, double dt) {
  (void)dt;
  spinner *s = data;
  s->angle += SPIN_SPEED;
  s->speed = SPIN_SPEED;
  s->ticks++;
  cw_node3d_set_rotation(n, (t3_vec3){ 0.3f, s->angle, 0 });
  cw_node_set(n, "color", cw_val_color(BOX_COLOR, 1));
}

static cw_node_type spinner_type = { .name = "Spinner", .parent = "MeshInstance3D", .data_size = sizeof(spinner),
                                     .props = spinner_props, .doc = "Turns about Y at SPIN_SPEED, painted BOX_COLOR.",
                                     .fixed_update = spin_fixed };

/* Every node type the game defines: the editor and cwhot call it. */
void cw_game_register_types(void);
void cw_game_register_types(void) { cw_node_type_register(&spinner_type); }

cw_app_desc cw_main(int argc, char **argv);
cw_app_desc cw_main(int argc, char **argv) {
  (void)argc;
  (void)argv;
  cw_scene_register_builtin_types();
  cw_game_register_types();
  static cw_app_desc base = { .title = "Agent Demo", .width = 480, .height = 270, .debug = true };
  return cw_scene_app("main.cwscene", &base);
}

/* A scene app that always leaves a program state no table has: one box under three kinds of light (a directional, a
 * point and a spot light), a light vector the unit tests' table lacks. It exists for tests/programs-carts.mjs, which
 * surveys its DUAL cart (tools/programs.mjs --carts) and must find the state; it is not a gate (a gate fails on a
 * missing state, which is the point here). It runs on GL and WebGPU like any scene app. */
#include "cartwheel/scene_file.h"

cw_app_desc cw_main(int argc, char **argv) {
  (void)argc;
  (void)argv;
  static cw_app_desc base = { .title = "programs missing", .width = 320, .height = 180, .debug = true };
  return cw_scene_app("main.cwscene", &base);
}

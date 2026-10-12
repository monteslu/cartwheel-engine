/* The debug channel's `environment` command: the sky and fog of the
 * current tree. */
#include "cartwheel/debug.h"
#include "environment_internal.h"

static void cmd_environment(int argc, const char **argv, cw_reply *r) {
  (void)argc;
  (void)argv;
  cw_tree *t = cw_current_tree();
  if (!t) {
    cw_reply_error(r, "no current tree");
    return;
  }
  cw_sky_state s;
  cw_sky_get_state(t, &s);
  cw_reply_printf(r, "{\"sky\":{\"active\":%s,\"time_of_day\":%.4f,\"sun\":[%.4f,%.4f,%.4f],\"elevation\":%.3f,"
                  "\"sun_color\":[%.4f,%.4f,%.4f],\"sun_energy\":%.4f,\"horizon\":[%.4f,%.4f,%.4f],\"cube_updates\":%d}",
                  s.active ? "true" : "false", s.time_of_day, s.sun_direction.x, s.sun_direction.y, s.sun_direction.z,
                  s.sun_elevation, s.sun_color.r, s.sun_color.g, s.sun_color.b, s.sun_energy, s.horizon.r, s.horizon.g,
                  s.horizon.b, s.cube_updates);
  cw_reply_printf(r, ",\"fog\":%s}", cw_tree_slot(t, "fog") ? "true" : "false");
}

void cw__environment_debug_init(void) {
  static bool done;
  if (done) return;
  done = true;
  cw_debug_command("environment", "environment: the sky's sun and updates, fog", cmd_environment);
}

/* three.c program tables. three.c draws every material with a program captured
 * from three.js r186, looked up by state in a table. The engine links
 * three.c's core table (the renderer's own programs: output pass,
 * backgrounds, PMREM, shadow casters); every other program comes from the
 * engine's table and the tables an app lists (cw_add_app ... PROGRAMS,
 * cmake/programs.cmake; the editor registers its own), whose
 * generated <table>_gl.c / <table>_wgpu.c are checked in beside their state
 * manifests. cw_add_app writes the app's <app>_programs.c, which defines
 * cw__app_programs() to register them; this is the weak default for a target
 * that lists none.
 *
 * A state no table has is a skipped draw, reported by
 * t3_renderer_generated_missing (the pipeline's warning and debug field
 * cw.three.missing; T3_MISSING_LOG=<file> natively). tools/programs.mjs adds
 * them to the app's manifest and regenerates. */
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>

#include "three.h"

/* the engine's own table (src/render/programs/cw_engine.txt): what it draws
 * on its own behalf, e.g. impostor baking */
extern const struct t3_gen_table cw_engine_gl;
#if CW_WGPU
extern const struct t3_gen_table cw_engine_wgpu;
#define CW_ENGINE_WGPU (&cw_engine_wgpu)
#else
#define CW_ENGINE_WGPU NULL
#endif

void cw__app_programs(void);
__attribute__((weak)) void cw__app_programs(void) {}
/* the app's target name (its generated <app>_programs.c), for the message that names the fix */
const char *cw__app_target_name(void);
__attribute__((weak)) const char *cw__app_target_name(void) { return "<app>"; }

/* states no table has, as the last frame left it: 0 when none. Set by the pipeline; the platform draws the banner. */
int32_t cw__missing_programs;
const char *cw__programs_fix_command(void);
const char *cw__programs_fix_command(void) {
  static char buf[160];
  snprintf(buf, sizeof buf, "node tools/programs.mjs %s", cw__app_target_name());
  return buf;
}

/* Before the first renderer: once, from the platform entry (and from the 3D
 * pipeline for hosts that create renderers without it). */
void cw__programs_register(void);
void cw__programs_register(void) {
  static bool done;
  if (done) return;
  done = true;
  cw__app_programs(); /* first: an app's programs win a tie */
  t3_register_program_table(&cw_engine_gl, CW_ENGINE_WGPU);
}

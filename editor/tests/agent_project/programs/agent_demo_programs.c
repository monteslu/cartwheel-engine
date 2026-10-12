/* The game's three.c program tables, for the host that runs this module:
 * cwhot asks for them at each (re)load and registers them (tools/cwhot/main.c).
 * programs/agent_demo.txt lists the states; agent_demo_gl.c / _wgpu.c are
 * generated from it (node <cartwheel>/tools/programs.mjs --table
 * programs/agent_demo.txt). */
#include "gen/programs.h"

extern const t3_gen_table agent_demo_gl, agent_demo_wgpu;

int cw__app_program_tables(const t3_gen_table **gl, const t3_gen_table **wgpu, int cap);
int cw__app_program_tables(const t3_gen_table **gl, const t3_gen_table **wgpu, int cap) {
  if (cap > 0) gl[0] = &agent_demo_gl, wgpu[0] = &agent_demo_wgpu;
  return 1;
}

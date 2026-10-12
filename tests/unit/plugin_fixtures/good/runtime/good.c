/* unit_plugin fixture: registers a node type, a method and a debug command. */
#include "cartwheel/debug.h"
#include "cartwheel/plugin.h"
#include "cartwheel/scene.h"

typedef struct { float value; } thing;
static const cw_prop props[] = { CW_PROP_FIELD(thing, value, CW_T_FLOAT, CW_PROP_DEFAULT, "a value"), CW_PROP_END };
static cw_node_type thing_type = { .name = "PluginThing", .parent = "Node3D", .data_size = sizeof(thing), .props = props,
                                   .doc = "fixture type (plugin good)" };
static int registers;
int good_register_count(void);
int good_register_count(void) { return registers; }

static void cmd(int argc, const char **argv, cw_reply *r) {
  (void)argc, (void)argv;
  cw_reply_printf(r, "{\"good\":%d}", registers);
}

static void reg(void) {
  registers++;
  cw_node_type_register(&thing_type);
  cw_debug_command("good_plugin", "good_plugin: the fixture plugin's register count", cmd);
}

CW_PLUGIN(good, .version = "2.3.4", .register_fn = reg);

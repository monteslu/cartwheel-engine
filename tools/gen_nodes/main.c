/* gen_nodes: every registered node type with its properties (type, default,
 * flags, range, enum names, doc), signals and parent, as JSON on stdout. The
 * build writes it to docs/nodes.json (tools/gen-api.mjs runs it), so node
 * properties are documented as data the same way functions are. */
#include <stdio.h>

#include "cartwheel/net.h"
#include "cartwheel/scene.h"

static void str(const char *s) {
  putchar('"');
  for (; s && *s; s++) {
    if (*s == '"' || *s == '\\') printf("\\%c", *s);
    else if (*s == '\n') printf("\\n");
    else putchar(*s);
  }
  putchar('"');
}

int main(void) {
  cw_scene_register_builtin_types();
  cw_net_register_types(); /* NetSynchronizer, NetSpawner (cartwheel/net.h) */
  printf("{\"generated_by\":\"tools/gen_nodes\",\"types\":[\n");
  for (int i = 0; i < cw_node_type_count(); i++) {
    const cw_node_type *t = cw_node_type_at(i);
    printf("%s{\"name\":", i ? ",\n" : "");
    str(t->name);
    printf(",\"parent\":");
    str(t->parent ? t->parent : "");
    printf(",\"doc\":");
    str(t->doc ? t->doc : "");
    printf(",\"signals\":[");
    for (int k = 0; t->signals && t->signals[k]; k++) {
      if (k) putchar(',');
      str(t->signals[k]);
    }
    printf("],\"properties\":[");
    cw_node n = cw_node_new(t->name, NULL);
    int first = 1;
    for (const cw_prop *p = t->props; p && p->name; p++) { /* this type's own properties; inherited ones are on its parent */
      cw_value v = { 0 };
      char def[512] = "";
      if (!CW_HANDLE_IS_NULL(n) && cw_node_get_prop(n, p->name, &v)) {
        if (p->type == CW_T_ENUM && p->enum_names && v.i >= 0) {
          int k = 0;
          while (p->enum_names[k] && k < v.i) k++;
          snprintf(def, sizeof def, "%s", p->enum_names[k] ? p->enum_names[k] : "?");
        } else {
          cw_value_format(&v, def, sizeof def);
        }
      }
      printf("%s{\"name\":", first ? "" : ",");
      first = 0;
      str(p->name);
      printf(",\"type\":\"%s\",\"default\":", cw_type_name(p->type));
      str(def);
      printf(",\"stored\":%s,\"readonly\":%s", (p->flags & CW_PROP_STORED) ? "true" : "false",
             (p->flags & CW_PROP_READONLY) ? "true" : "false");
      if (p->min != 0 || p->max != 0) printf(",\"min\":%g,\"max\":%g", p->min, p->max);
      if (p->enum_names) {
        printf(",\"enum\":[");
        for (int k = 0; p->enum_names[k]; k++) {
          if (k) putchar(',');
          str(p->enum_names[k]);
        }
        putchar(']');
      }
      printf(",\"doc\":");
      str(p->doc ? p->doc : "");
      putchar('}');
    }
    printf("]}");
    if (!CW_HANDLE_IS_NULL(n)) cw_node_free(n);
  }
  printf("\n]}\n");
  return 0;
}

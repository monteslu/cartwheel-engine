/* Registration owners (M5.2 plugins): every registration (command, panel,
 * menu item, toolbar button, inspector plugin, gizmo, event subscription)
 * remembers who made it, so a plugin unloaded or reloaded at runtime takes
 * exactly its own contributions with it. Owner 0 is the editor itself (the
 * core and the built-in modules); the plugin manager pushes a plugin's owner
 * around its init, frame and shutdown. */
#include "internal.h"

#define MAX_OWNERS 64
static const char *owner_names[MAX_OWNERS] = { "editor" };
static int owner_count = 1;
static int stack[16];
static int depth;

int cwed__owner;

int cwed_owner_id(const char *name) {
  name = cw_intern(name);
  for (int i = 1; i < owner_count; i++)
    if (owner_names[i] == name) return i;
  CW_ASSERT(owner_count < MAX_OWNERS, "too many registration owners");
  owner_names[owner_count] = name;
  return owner_count++;
}

void cwed_owner_push(int id) {
  CW_ASSERT(depth < (int)CW_ARRAY_LEN(stack), "cwed_owner_push nested too deep");
  stack[depth++] = cwed__owner;
  cwed__owner = id;
}

void cwed_owner_pop(void) {
  CW_ASSERT(depth > 0, "cwed_owner_pop without a push");
  cwed__owner = stack[--depth];
}

int cwed_owner_current(void) { return cwed__owner; }
const char *cwed_owner_name(int id) { return id >= 0 && id < owner_count ? owner_names[id] : NULL; }

cwed_owner_stats cwed_owner_contributions(int id) {
  cwed_owner_stats s = { 0 };
  cwed__commands_owned(id, &s);
  cwed__dock_owned(id, &s);
  cwed__shell_owned(id, &s);
  cwed__extensions_owned(id, &s);
  cwed__events_owned(id, &s);
  return s;
}

void cwed_owner_remove(int id) {
  if (id <= 0) return; /* the editor's own registrations stay */
  cwed__dock_remove_owner(id);
  cwed__shell_remove_owner(id);
  cwed__extensions_remove_owner(id);
  cwed__events_remove_owner(id);
  cwed__commands_remove_owner(id);
}

/* Trigger nodes: "when <source> emits <signal> then <action>" rules stored in
 * scenes (cartwheel/triggers.h has the semantics). */
#include <stdio.h>
#include <string.h>

#include "cartwheel/scene_file.h"
#include "cartwheel/triggers.h"
#include "scene_internal.h"

typedef struct trigger {
  bool enabled;
  const char *source, *signal, *filter_group;
  int action;
  const char *target, *member, *value;
  bool once;
  int32_t fired;
  /* runtime */
  uint64_t conn;
  cw_node connected_to;
  bool readied;
  int slot; /* 1-based index in slots[]: the connection callback finds the trigger by it */
} trigger;

/* The signal callback gets a user pointer, not the connection target, and a
 * 64-bit handle does not fit a wasm32 pointer: each trigger owns a slot that
 * maps a small number back to its handle (checked against the emitter). */
static CW_ARRAY(cw_node) slots;
static CW_ARRAY(int) free_slots;

static const char *const action_names[] = { "SET", "CALL", "EMIT", "SPAWN", "FREE", "CHANGE_SCENE", NULL };
static cw_node_type trigger_type;

static const char *str(const char *s) { return s ? s : ""; }

static bool type_declares_signal(const cw_node_type *t, const char *sig) {
  for (; t; t = t->base)
    for (const char *const *s = t->signals; s && *s; s++)
      if (!strcmp(*s, sig)) return true;
  return false;
}

static void disconnect(trigger *t) {
  if (t->conn) cw_disconnect(t->conn);
  t->conn = 0;
  t->connected_to = (cw_node){ 0 };
}

static void on_signal(cw_node emitter, const cw_value *args, int argc, void *user);

static void connect(cw_node n, trigger *t) {
  disconnect(t);
  if (!t->enabled || !t->signal || !*t->signal) return;
  cw_node src = cw_node_get(n, str(t->source)[0] ? t->source : "..");
  char path[256];
  if (CW_HANDLE_IS_NULL(src)) {
    cw_node_path(n, (cw_node){ 0 }, path, sizeof path);
    cw_warn("trigger %s: no source node '%s' (set its source to a path relative to the trigger, e.g. \"..\" or \"../Door\")",
            path, str(t->source));
    return;
  }
  if (!type_declares_signal(cw_node_get_type(src), t->signal)) {
    cw_node_path(n, (cw_node){ 0 }, path, sizeof path);
    cw_warn("trigger %s: %s (%s) declares no signal '%s' (the editor's Triggers panel lists a node's signals)", path,
            cw_node_name(src), cw_node_get_type(src)->name, t->signal);
    return;
  }
  t->conn = cw_connect(src, t->signal, n, NULL, on_signal, (void *)(uintptr_t)t->slot, CW_CONNECT_DEFERRED);
  t->connected_to = src;
}

static bool parse_for(const char *text, cw_type want, const char *const *enum_names, cw_value *out) {
  if (want == CW_T_ENUM && enum_names) {
    *out = cw_val_enum(text);
    return true;
  }
  if (want == CW_T_STRING) {
    if (text[0] == '"' && cw_value_parse(text, CW_T_STRING, out)) return true;
    *out = cw_val_string(text);
    return true;
  }
  return cw_value_parse(text, want, out);
}

static cw_type type_by_name(const char *name, int len) {
  for (int k = 1; k < CW_T_COUNT; k++)
    if ((int)strlen(cw_type_name((cw_type)k)) == len && !strncmp(cw_type_name((cw_type)k), name, (size_t)len)) return (cw_type)k;
  return CW_T_NONE;
}

static bool run_action(cw_node n, trigger *t, const cw_value *args, int argc) {
  char path[256];
  cw_node_path(n, (cw_node){ 0 }, path, sizeof path);
  if (t->action == CW_TRIGGER_CHANGE_SCENE) {
    if (!str(t->value)[0]) { cw_warn("trigger %s: CHANGE_SCENE needs value = the scene path", path); return false; }
    return cw_scene_app_change_scene(t->value, false);
  }
  cw_node target = cw_node_get(n, str(t->target)[0] ? t->target : "..");
  if (CW_HANDLE_IS_NULL(target)) {
    cw_warn("trigger %s: no target node '%s' (a path relative to the trigger, e.g. \"..\" or \"../Lamp\")", path, str(t->target));
    return false;
  }
  switch (t->action) {
  case CW_TRIGGER_SET: {
    const cw_prop *p = cw_node_find_prop(target, str(t->member), NULL);
    if (!p) { cw_warn("trigger %s: %s has no property '%s'", path, cw_node_name(target), str(t->member)); return false; }
    cw_value v;
    if (!parse_for(str(t->value), p->type, p->enum_names, &v)) {
      cw_warn("trigger %s: value '%s' is not a %s for %s.%s", path, str(t->value), cw_type_name(p->type), cw_node_name(target), p->name);
      return false;
    }
    return cw_node_set(target, p->name, v);
  }
  case CW_TRIGGER_CALL: {
    if (!str(t->value)[0]) return cw_node_call(target, str(t->member), args, argc, NULL);
    const cw_method *m = cw_node_find_method(target, str(t->member));
    cw_type want = CW_T_STRING;
    if (m && m->args && *m->args && strcmp(m->args, "...")) {
      const char *comma = strchr(m->args, ',');
      want = type_by_name(m->args, comma ? (int)(comma - m->args) : (int)strlen(m->args));
    }
    cw_value v;
    if (!parse_for(t->value, want, NULL, &v)) {
      cw_warn("trigger %s: value '%s' is not a %s for %s()", path, t->value, cw_type_name(want), str(t->member));
      return false;
    }
    return cw_node_call(target, str(t->member), &v, 1, NULL);
  }
  case CW_TRIGGER_EMIT:
    if (!str(t->member)[0]) { cw_warn("trigger %s: EMIT needs member = the signal name", path); return false; }
    cw_emit(target, t->member, args, argc);
    return true;
  case CW_TRIGGER_SPAWN: {
    if (!str(t->value)[0]) { cw_warn("trigger %s: SPAWN needs value = the scene path", path); return false; }
    cw_node inst = cw_scene_instance_path(t->value);
    if (CW_HANDLE_IS_NULL(inst)) return false;
    if (str(t->member)[0]) cw_node_set_name(inst, t->member);
    cw_node_add_child(target, inst);
    return true;
  }
  case CW_TRIGGER_FREE:
    cw_node_free(target);
    return true;
  }
  return false;
}

static bool fire(cw_node n, trigger *t, const cw_value *args, int argc) {
  if (!t->enabled || (t->once && t->fired > 0)) return false;
  if (str(t->filter_group)[0]) {
    if (argc < 1 || args[0].type != CW_T_NODE || !cw_node_valid(args[0].node) || !cw_node_in_group(args[0].node, t->filter_group))
      return false;
  }
  t->fired++;
  bool ok = run_action(n, t, args, argc);
  if (t->once && cw_node_valid(n)) disconnect(t);
  return ok;
}

static void on_signal(cw_node emitter, const cw_value *args, int argc, void *user) {
  int slot = (int)(uintptr_t)user;
  if (slot < 1 || slot > slots.count) return;
  cw_node n = slots.data[slot - 1];
  trigger *t = cw_node_valid(n) ? cw_node_data_t(n, &trigger_type) : NULL;
  if (!t || t->connected_to.id != emitter.id) return; /* freed (slot reused) or reconnected meanwhile */
  fire(n, t, args, argc);
}

bool cw_trigger_fire(cw_node n, const cw_value *args, int argc) {
  trigger *t = cw_node_data_t(n, &trigger_type);
  if (!t) return false;
  return fire(n, t, args, argc);
}

static void t_init(cw_node n, void *data) {
  trigger *t = data;
  if (free_slots.count) {
    t->slot = free_slots.data[--free_slots.count];
    slots.data[t->slot - 1] = n;
  } else {
    cw_array_push(&slots, n);
    t->slot = slots.count;
  }
  t->enabled = true;
  t->source = cw_intern("..");
  t->target = cw_intern("..");
  t->signal = t->filter_group = t->member = t->value = cw_intern("");
}
static void t_destroy(cw_node n, void *data) {
  (void)n;
  trigger *t = data;
  disconnect(t);
  if (t->slot) {
    slots.data[t->slot - 1] = (cw_node){ 0 };
    cw_array_push(&free_slots, t->slot);
  }
}
static void t_ready(cw_node n, void *data) {
  trigger *t = data;
  t->readied = true;
  connect(n, t);
}
static void t_enter(cw_node n, void *data) {
  trigger *t = data;
  if (t->readied) connect(n, t); /* re-entering after a reparent */
}
static void t_exit(cw_node n, void *data) {
  (void)n;
  disconnect(data);
}
static void t_changed(cw_node n, void *data, const cw_prop *p) {
  trigger *t = data;
  if (t->readied && cw_node_in_tree(n) &&
      (!strcmp(p->name, "source") || !strcmp(p->name, "signal") || !strcmp(p->name, "enabled")))
    connect(n, t);
}

static const cw_prop trigger_props[] = {
  CW_PROP_FIELD(trigger, enabled, CW_T_BOOL, CW_PROP_DEFAULT, "false = the rule never fires (another trigger may SET it true)."),
  CW_PROP_FIELD(trigger, source, CW_T_STRING, CW_PROP_DEFAULT, "The node whose signal fires the rule, relative to the trigger (\"..\" = its parent)."),
  CW_PROP_FIELD(trigger, signal, CW_T_STRING, CW_PROP_DEFAULT, "The source's signal (body_entered, pressed, animation_finished, ...)."),
  CW_PROP_FIELD(trigger, filter_group, CW_T_STRING, CW_PROP_DEFAULT, "Fire only when the signal's first argument is a node in this group (empty = always)."),
  { .name = "action", .type = CW_T_ENUM, .flags = CW_PROP_DEFAULT, .offset = offsetof(trigger, action), .size = sizeof(int),
    .enum_names = action_names, .doc = "What happens: SET a property, CALL a method, EMIT a signal, SPAWN a scene, FREE the target, CHANGE_SCENE." },
  CW_PROP_FIELD(trigger, target, CW_T_STRING, CW_PROP_DEFAULT, "The node acted on, relative to the trigger (\"..\" = its parent)."),
  CW_PROP_FIELD(trigger, member, CW_T_STRING, CW_PROP_DEFAULT, "SET: property; CALL: method; EMIT: signal; SPAWN: the new node's name (optional)."),
  CW_PROP_FIELD(trigger, value, CW_T_STRING, CW_PROP_DEFAULT, "SET: the value as scene text; CALL: one argument (empty = pass the signal's); SPAWN / CHANGE_SCENE: a scene path."),
  CW_PROP_FIELD(trigger, once, CW_T_BOOL, CW_PROP_DEFAULT, "Fire at most once."),
  CW_PROP_FIELD(trigger, fired, CW_T_INT, CW_PROP_EDITOR | CW_PROP_READONLY, "How many times the rule ran (not saved)."),
  CW_PROP_END,
};

static cw_node_type trigger_type = { .name = "Trigger", .parent = "Node", .data_size = sizeof(trigger), .props = trigger_props,
                                     .doc = "A data-driven rule: when `source` emits `signal`, do `action` on `target` (cartwheel/triggers.h).",
                                     .init = t_init, .destroy = t_destroy, .ready = t_ready, .enter_tree = t_enter, .exit_tree = t_exit,
                                     .prop_changed = t_changed };

void cw_triggers_register(void) { cw_node_type_register(&trigger_type); }

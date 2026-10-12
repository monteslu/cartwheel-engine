/* Extension registries (inspector plugins, gizmos, the play backend) and
 * the play.* commands. */
#include "internal.h"

/* ── shared drop targets ──────────────────────────────────────────────── */

typedef struct { cw_node target; cwed_drop_desc d; int owner; } drop_entry;
static CW_ARRAY(drop_entry) drops;

static bool drop_can(cw_node t, const cw_ui_drag *g, cw_vec2 local, cw_rect *hl, void *user) {
  (void)user;
  for (int i = 0; i < drops.count; i++) {
    drop_entry *e = &drops.data[i];
    if (e->target.id == t.id && e->d.kind == g->kind && e->d.can_drop && e->d.can_drop(t, g, local, hl, e->d.user)) return true;
  }
  return false;
}
static void drop_do(cw_node t, const cw_ui_drag *g, cw_vec2 local, void *user) {
  (void)user;
  for (int i = 0; i < drops.count; i++) {
    drop_entry *e = &drops.data[i];
    cw_rect hl = { 0 };
    if (e->target.id == t.id && e->d.kind == g->kind && e->d.can_drop && e->d.can_drop(t, g, local, &hl, e->d.user)) {
      if (e->d.drop) e->d.drop(t, g, local, e->d.user);
      return;
    }
  }
}

void cwed_drop_add(cw_node target, const cwed_drop_desc *d) {
  for (int i = drops.count - 1; i >= 0; i--)
    if (!cw_node_valid(drops.data[i].target)) cw_array_remove_swap(&drops, i);
  const char *kind = cw_intern(d->kind ? d->kind : "");
  for (int i = 0; i < drops.count; i++) {
    drop_entry *e = &drops.data[i];
    if (e->target.id == target.id && e->d.kind == kind && e->d.can_drop == d->can_drop && e->d.user == d->user) return;
  }
  drop_entry e = { target, *d, cwed__owner };
  e.d.kind = kind;
  cw_array_push(&drops, e);
  cw_ui_set_drop_target(target, &(cw_ui_drop_fns){ .can_drop = drop_can, .drop = drop_do });
}

/* ── inspector plugins ────────────────────────────────────────────────── */

static CW_ARRAY(const cwed_inspector_plugin *) inspectors;
static CW_ARRAY(int) inspector_owner;

void cwed_inspector_register(const cwed_inspector_plugin *p) {
  cw_array_push(&inspectors, p);
  cw_array_push(&inspector_owner, cwed__owner);
}
int cwed_inspector_count(void) { return inspectors.count; }
const cwed_inspector_plugin *cwed_inspector_at(int i) { return i >= 0 && i < inspectors.count ? inspectors.data[i] : NULL; }
const cwed_inspector_plugin *cwed_inspector_for(cw_node n, const cw_prop *p) {
  const cwed_inspector_plugin *best = NULL;
  for (int i = 0; i < inspectors.count; i++) {
    const cwed_inspector_plugin *q = inspectors.data[i];
    if ((!best || q->priority > best->priority) && q->handles && q->handles(n, p)) best = q;
  }
  return best;
}

bool cwed_prop_commit(const char *prop, const char *value_text, bool final) {
  int k = cwed_selection_count();
  if (!k) return false;
  cwed_doc *d = cwed_doc_active();
  bool ok;
  if (k == 1) {
    ok = cwed_runf("node.set path=%s prop=%s value=%s merge=%s", cwed_quote(cwed_doc_node_path(d, cwed_selection_get(0))),
                   cwed_quote(prop), cwed_quote(value_text), cwed_quote(cw_frame_printf("inspector:%s", prop)));
  } else {
    size_t cap = 64;
    for (int i = 0; i < k; i++) cap += strlen(cwed_doc_node_path(d, cwed_selection_get(i))) + 1;
    char *paths = cw_frame_alloc(cap);
    paths[0] = 0;
    for (int i = 0; i < k; i++) {
      if (i) strcat(paths, ",");
      strcat(paths, cwed_doc_node_path(d, cwed_selection_get(i)));
    }
    ok = cwed_runf("node.set_many paths=%s prop=%s value=%s merge=%s", cwed_quote(paths), cwed_quote(prop),
                   cwed_quote(value_text), cwed_quote(cw_frame_printf("inspector:%s", prop)));
  }
  if (final) cwed_history_seal();
  return ok;
}

/* ── gizmos ───────────────────────────────────────────────────────────── */

static CW_ARRAY(const cwed_gizmo_desc *) gizmos;
static CW_ARRAY(int) gizmo_owner;
void cwed_gizmo_register(const cwed_gizmo_desc *g) {
  cw_array_push(&gizmos, g);
  cw_array_push(&gizmo_owner, cwed__owner);
}

/* ── registration owners (plugins) ────────────────────────────────────── */

void cwed__extensions_owned(int owner, cwed_owner_stats *s) {
  for (int i = 0; i < inspectors.count; i++) s->inspectors += inspector_owner.data[i] == owner;
  for (int i = 0; i < gizmos.count; i++) s->gizmos += gizmo_owner.data[i] == owner;
}
void cwed__extensions_remove_owner(int owner) {
  int w = 0;
  for (int i = 0; i < inspectors.count; i++)
    if (inspector_owner.data[i] != owner) inspectors.data[w] = inspectors.data[i], inspector_owner.data[w++] = inspector_owner.data[i];
  inspectors.count = inspector_owner.count = w;
  w = 0;
  for (int i = 0; i < gizmos.count; i++)
    if (gizmo_owner.data[i] != owner) gizmos.data[w] = gizmos.data[i], gizmo_owner.data[w++] = gizmo_owner.data[i];
  gizmos.count = gizmo_owner.count = w;
  for (int i = drops.count - 1; i >= 0; i--) /* drop handlers a plugin put on other panels */
    if (drops.data[i].owner == owner) cw_array_remove_swap(&drops, i);
}
int cwed_gizmo_count(void) { return gizmos.count; }
const cwed_gizmo_desc *cwed_gizmo_at(int i) { return i >= 0 && i < gizmos.count ? gizmos.data[i] : NULL; }

/* ── play-in-editor ───────────────────────────────────────────────────── */

static const cwed_play_backend *play;
static bool paused;

void cwed_play_register(const cwed_play_backend *b) { play = b; }
const cwed_play_backend *cwed_play_backend_get(void) { return play; }

static bool need_backend(cwed_call *c) {
  if (play) return true;
  return cwed_fail(c, "no play backend is registered: play-in-editor (the game in its own process, crash-isolated) "
                      "comes from the M4.3 module through cwed_play_register");
}

static bool play_enabled(void) { return play != NULL; }

static bool cmd_start(cwed_call *c) {
  if (!need_backend(c)) return false;
  const char *scene = cwed_arg(c, "scene");
  cwed_doc *d = cwed_doc_active();
  if (!*scene) scene = d && d->path[0] ? d->path : cwed_project_main_scene();
  if (!*scene) return cwed_fail(c, "nothing to play: save the scene first (scene.save path=...), or set main_scene in project.cwproj");
  /* the game reads the project from disk / the project layer: save what is open first */
  if (d && d->path[0] && cwed_doc_dirty(d) && !cwed_run("scene.save", NULL)) return cwed_fail(c, "could not save %s before playing", d->title);
  char err[512] = "";
  if (!play->start(scene, err, sizeof err)) return cwed_fail(c, "%s", err[0] ? err : "the game did not start");
  paused = false;
  cwed_json_key(cwed_reply(c), "scene");
  cwed_json_str(cwed_reply(c), scene);
  return true;
}
static bool cmd_stop(cwed_call *c) {
  if (!need_backend(c)) return false;
  play->stop();
  return true;
}
static bool cmd_pause(cwed_call *c) {
  if (!need_backend(c)) return false;
  if (play->paused) paused = play->paused();
  paused = cwed_arg_given(c, "on") ? cwed_arg_bool(c, "on") : !paused;
  play->set_paused(paused);
  cwed_json_printf(cwed_reply(c), "\"paused\":%s", paused ? "true" : "false");
  return true;
}
static bool cmd_step(cwed_call *c) {
  if (!need_backend(c)) return false;
  play->step((int)cwed_arg_int(c, "ticks"));
  return true;
}

void cwed__play_frame(void) {
  if (play && play->poll) play->poll();
}

void cwed__play_commands(void) {
  static const cwed_param start_p[] = { { "scene", CWED_ARG_STRING, "", "scene to run (default: the active one, else the main scene)" }, { 0 } };
  static const cwed_param pause_p[] = { { "on", CWED_ARG_BOOL, "true", "pause (true) or resume (false); default toggles" }, { 0 } };
  static const cwed_param step_p[] = { { "ticks", CWED_ARG_INT, "1", "fixed ticks to advance while paused" }, { 0 } };
  cwed_command_register(&(cwed_command_desc){ .name = "play.start", .title = "Play Scene", .params = start_p, .shortcut = "f5",
    .doc = "Run a scene as the game, in its own process (M4.3 backend).", .exec = cmd_start, .enabled = play_enabled });
  cwed_command_register(&(cwed_command_desc){ .name = "play.stop", .title = "Stop Game", .shortcut = "f8",
    .doc = "Stop the running game.", .exec = cmd_stop, .enabled = play_enabled });
  cwed_command_register(&(cwed_command_desc){ .name = "play.pause", .title = "Pause Game", .params = pause_p, .shortcut = "f7",
    .doc = "Pause or resume the running game.", .exec = cmd_pause, .enabled = play_enabled });
  cwed_command_register(&(cwed_command_desc){ .name = "play.step", .title = "Step Game", .params = step_p, .shortcut = "f6",
    .doc = "Advance the paused game by fixed ticks.", .exec = cmd_step, .enabled = play_enabled });
}

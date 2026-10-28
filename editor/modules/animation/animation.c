/* Animation commands: build an AnimationPlayer's animations from editor
 * commands, the way a scene file declares them ([animation], [track] and key
 * lines: cartwheel/anim.h), so a scene with animations can be rebuilt by
 * driving only editor commands (tools/scene-to-ed.mjs).
 *
 * Commands (undoable, on the active scene):
 *   anim.new player=<path> name=<n> length=<s> [loop=NONE|LOOP|PING_PONG]
 *   anim.track_add player=<path> anim=<n> [kind=value|method|event] [path=<node:prop>] [interp=NEAREST|LINEAR|CUBIC|HERMITE]
 *   anim.track_set player=<path> anim=<n> track=<i> [interp=..] [rot=SLERP|NLERP] [enabled=true|false]
 *   anim.key_add player=<path> anim=<n> track=<i> time=<s> value=<key text>
 *     (the key text as a scene file writes it: `vec3(0, 1, 0)`,
 *     `vec3(0, 1, 0) in=vec3(0, 0, 0) out=vec3(0, 0, 0)` for a HERMITE track,
 *     `name(arg, ...)` for a method track, `"event"` for an event track)
 *   anim.text player=<path> anim=<n>   the animation as scene-file text (a query)
 *
 * Each edit rebuilds the animation from its text (cw_anim_to_text, edited,
 * cw_anim_from_text): the same parser a scene file goes through, so what a
 * command accepts is what the file accepts. Undo puts the text from before
 * back (or removes an animation anim.new added). */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cartwheel/anim.h"
#include "cartwheel/scene_file.h"
#include "cwed/editor.h"

typedef struct {
  cw_node player;
  const char *name; /* interned */
  char *before;     /* the animation's text before this edit; NULL: it did not exist */
} anim_state;

static void free_state(cwed_call *c, void *state) {
  (void)c;
  anim_state *s = state;
  if (s) cw_free(s->before);
  cw_free(s);
}

static cw_node player_arg(cwed_call *c) {
  cw_node p = cwed_arg_node(c, "player");
  if (CW_HANDLE_IS_NULL(p)) return p;
  if (!cw_node_is(p, "AnimationPlayer")) {
    cwed_fail(c, "'%s' is a %s, not an AnimationPlayer", cwed_arg(c, "player"), cw_node_get_type(p)->name);
    return (cw_node){ 0 };
  }
  return p;
}

/* the edit's state, with the text from before it (refreshed on every run: a redo
 * runs exec again on the state undo left) */
static anim_state *begin(cwed_call *c, cw_node player, const char *name) {
  anim_state *s = cwed_call_state(c);
  if (!s) {
    s = cw_calloc(1, sizeof *s, CW_MEM_EDITOR);
    cwed_call_set_state(c, s);
  }
  cw_free(s->before);
  s->player = player;
  s->name = cw_intern(name);
  cw_anim a = cw_anim_player_get(player, name);
  s->before = cw_anim_valid(a) ? cw_anim_to_text(a, ".") : NULL;
  return s;
}

/* replace the animation with the one this text declares */
static bool apply(cwed_call *c, anim_state *s, const char *text) {
  cw_anim a = cw_anim_from_text(text);
  if (!cw_anim_valid(a)) return cwed_fail(c, "the animation did not parse (the log says why): %s", text);
  cw_anim_player_add(s->player, s->name, a);
  cw_anim_release(a); /* the player holds it */
  cwed_json *j = cwed_reply(c);
  cwed_json_key(j, "anim");
  cwed_json_str(j, s->name);
  cwed_json_key(j, "tracks");
  cwed_json_printf(j, "%d", cw_anim_track_count(a));
  return true;
}

static void undo(cwed_call *c) {
  anim_state *s = cwed_call_state(c);
  if (!s || !cw_node_valid(s->player)) return;
  if (!s->before) {
    cw_anim_player_remove(s->player, s->name);
    return;
  }
  cw_anim a = cw_anim_from_text(s->before);
  if (!cw_anim_valid(a)) return;
  cw_anim_player_add(s->player, s->name, a);
  cw_anim_release(a);
}

static bool in_list(const char *v, const char *const *list) {
  for (int i = 0; list[i]; i++)
    if (!strcmp(v, list[i])) return true;
  return false;
}
static const char *const LOOPS[] = { "NONE", "LOOP", "PING_PONG", NULL };
static const char *const INTERPS[] = { "NEAREST", "LINEAR", "CUBIC", "HERMITE", NULL };
static const char *const KINDS[] = { "value", "method", "event", NULL };
static const char *const ROTS[] = { "SLERP", "NLERP", NULL };

/* a quoted scene-file string */
static void put_quoted(char *out, size_t cap, const char *s) {
  size_t n = 0;
  if (n < cap) out[n++] = '"';
  for (; *s && n + 3 < cap; s++) {
    if (*s == '"' || *s == '\\') out[n++] = '\\';
    out[n++] = *s;
  }
  if (n < cap) out[n++] = '"';
  out[n < cap ? n : cap - 1] = 0;
}

/* the existing animation's text, or a refusal naming anim.new */
static char *existing(cwed_call *c, anim_state *s) {
  if (!s->before) {
    cwed_fail(c, "'%s' has no animation '%s' (anim.new player=%s name=%s length=1 makes one)", cwed_arg(c, "player"), s->name,
              cwed_arg(c, "player"), s->name);
    return NULL;
  }
  return s->before;
}

/* ── anim.new ─────────────────────────────────────────────────────────── */

static bool new_exec(cwed_call *c) {
  cw_node p = player_arg(c);
  if (CW_HANDLE_IS_NULL(p)) return false;
  const char *name = cwed_arg(c, "name"), *loop = cwed_arg(c, "loop");
  if (!*name) return cwed_fail(c, "give the animation a name");
  if (!in_list(loop, LOOPS)) return cwed_fail(c, "loop=%s: use NONE, LOOP or PING_PONG", loop);
  double len = cwed_arg_float(c, "length");
  if (!(len >= 0)) return cwed_fail(c, "length is seconds (>= 0)");
  anim_state *s = begin(c, p, name);
  if (s->before) return cwed_fail(c, "'%s' already has an animation '%s'", cwed_arg(c, "player"), name);
  char q[512], text[768];
  put_quoted(q, sizeof q, name);
  snprintf(text, sizeof text, "[animation name=%s player=\".\" length=%.9g%s%s]\n", q, len, strcmp(loop, "NONE") ? " loop=" : "",
           strcmp(loop, "NONE") ? loop : "");
  return apply(c, s, text);
}

/* ── anim.track_add / anim.track_set ──────────────────────────────────── */

static bool track_add_exec(cwed_call *c) {
  cw_node p = player_arg(c);
  if (CW_HANDLE_IS_NULL(p)) return false;
  const char *kind = cwed_arg(c, "kind"), *path = cwed_arg(c, "path"), *interp = cwed_arg(c, "interp");
  if (!in_list(kind, KINDS)) return cwed_fail(c, "kind=%s: use value, method or event", kind);
  if (strcmp(kind, "event") && !*path) return cwed_fail(c, "a %s track needs path=<node:property> (or a node, for method)", kind);
  if (!in_list(interp, INTERPS)) return cwed_fail(c, "interp=%s: use NEAREST, LINEAR, CUBIC or HERMITE", interp);
  anim_state *s = begin(c, p, cwed_arg(c, "anim"));
  const char *before = existing(c, s);
  if (!before) return false;
  size_t cap = strlen(before) + strlen(path) * 2 + 128;
  char *text = cw_alloc(cap, CW_MEM_EDITOR), q[1024] = "";
  if (strcmp(kind, "event")) put_quoted(q, sizeof q, path);
  snprintf(text, cap, "%s[track type=%s%s%s%s%s]\n", before, kind, *q ? " path=" : "", q,
           strcmp(interp, "LINEAR") && !strcmp(kind, "value") ? " interp=" : "",
           strcmp(interp, "LINEAR") && !strcmp(kind, "value") ? interp : "");
  bool ok = apply(c, s, text);
  cw_free(text);
  return ok;
}

/* the start of the nth "[track" line of text, NULL when there is none */
static char *track_line(char *text, int64_t nth) {
  int64_t k = -1;
  for (char *l = text; l && *l;) {
    while (*l == ' ' || *l == '\t') l++;
    if (!strncmp(l, "[track", 6) && ++k == nth) return l;
    l = strchr(l, '\n');
    if (l) l++;
  }
  return NULL;
}

static bool track_set_exec(cwed_call *c) {
  cw_node p = player_arg(c);
  if (CW_HANDLE_IS_NULL(p)) return false;
  anim_state *s = begin(c, p, cwed_arg(c, "anim"));
  if (!existing(c, s)) return false;
  int64_t nth = cwed_arg_int(c, "track");
  char *text = cw_alloc(strlen(s->before) + 256, CW_MEM_EDITOR);
  strcpy(text, s->before);
  char *h = track_line(text, nth);
  if (!h) {
    cw_free(text);
    return cwed_fail(c, "'%s' has no track %lld", s->name, (long long)nth);
  }
  char *he = strchr(h, ']');
  if (!he) {
    cw_free(text);
    return cwed_fail(c, "track %lld has no header end", (long long)nth);
  }
  char hdr[1200];
  snprintf(hdr, sizeof hdr, "%.*s", (int)(he - h + 1), h);
  const char *type = cw_scene_attr(hdr, "type"), *tp = cw_scene_attr(hdr, "path"), *in = cw_scene_attr(hdr, "interp"),
             *rot = cw_scene_attr(hdr, "rot"), *en = cw_scene_attr(hdr, "enabled");
  if (cwed_arg_given(c, "interp")) in = cwed_arg(c, "interp");
  if (cwed_arg_given(c, "rot")) rot = cwed_arg(c, "rot");
  if (cwed_arg_given(c, "enabled")) en = cwed_arg_bool(c, "enabled") ? "true" : "false";
  if (in && !in_list(in, INTERPS)) {
    cw_free(text);
    return cwed_fail(c, "interp=%s: use NEAREST, LINEAR, CUBIC or HERMITE", in);
  }
  if (rot && !in_list(rot, ROTS)) {
    cw_free(text);
    return cwed_fail(c, "rot=%s: use SLERP or NLERP", rot);
  }
  char q[1024] = "", nh[1400];
  if (tp) put_quoted(q, sizeof q, tp);
  snprintf(nh, sizeof nh, "[track type=%s%s%s%s%s%s%s]", type ? type : "value", tp ? " path=" : "", q,
           in && strcmp(in, "LINEAR") ? " interp=" : "", in && strcmp(in, "LINEAR") ? in : "",
           rot && !strcmp(rot, "NLERP") ? " rot=NLERP" : "", en && !strcmp(en, "false") ? " enabled=false" : "");
  size_t cap = strlen(text) + strlen(nh) + 8;
  char *out = cw_alloc(cap, CW_MEM_EDITOR);
  snprintf(out, cap, "%.*s%s%s", (int)(h - text), text, nh, he + 1);
  cw_free(text);
  bool ok = apply(c, s, out);
  cw_free(out);
  return ok;
}

/* ── anim.key_add ─────────────────────────────────────────────────────── */

static bool key_add_exec(cwed_call *c) {
  cw_node p = player_arg(c);
  if (CW_HANDLE_IS_NULL(p)) return false;
  anim_state *s = begin(c, p, cwed_arg(c, "anim"));
  if (!existing(c, s)) return false;
  int64_t nth = cwed_arg_int(c, "track");
  const char *value = cwed_arg(c, "value"), *time = cwed_arg(c, "time");
  char *end;
  double t = strtod(time, &end);
  if (end == time || !(t >= 0)) return cwed_fail(c, "time=%s: seconds (>= 0)", time);
  if (!*value || strchr(value, '\n')) return cwed_fail(c, "value is one key line's text (no newlines)");
  char *text = cw_alloc(strlen(s->before) + 8, CW_MEM_EDITOR);
  strcpy(text, s->before);
  char *h = track_line(text, nth);
  if (!h) {
    cw_free(text);
    return cwed_fail(c, "'%s' has no track %lld (anim.track_add adds one)", s->name, (long long)nth);
  }
  /* the key goes at the end of the track's lines: before the next [track] or the end */
  char *next = track_line(text, nth + 1);
  size_t at = next ? (size_t)(next - text) : strlen(text);
  size_t cap = strlen(text) + strlen(value) + 64;
  char *out = cw_alloc(cap, CW_MEM_EDITOR);
  snprintf(out, cap, "%.*s%s%.9g = %s\n%s", (int)at, text, at && text[at - 1] != '\n' ? "\n" : "", t, value, text + at);
  cw_free(text);
  bool ok = apply(c, s, out);
  cw_free(out);
  return ok;
}

/* ── anim.text ────────────────────────────────────────────────────────── */

static bool text_exec(cwed_call *c) {
  cw_node p = player_arg(c);
  if (CW_HANDLE_IS_NULL(p)) return false;
  const char *name = cwed_arg(c, "anim");
  cw_anim a = cw_anim_player_get(p, name);
  if (!cw_anim_valid(a)) return cwed_fail(c, "'%s' has no animation '%s'", cwed_arg(c, "player"), name);
  char *t = cw_anim_to_text(a, cwed_arg(c, "player"));
  cwed_json_key(cwed_reply(c), "text");
  cwed_json_str(cwed_reply(c), t);
  cw_free(t);
  return true;
}

static void init(void) {
  const uint32_t U = CWED_CMD_UNDO | CWED_CMD_SCENE;
  static const cwed_param new_p[] = {
    { "player", CWED_ARG_PATH, NULL, "the AnimationPlayer" },
    { "name", CWED_ARG_STRING, NULL, "the animation's name" },
    { "length", CWED_ARG_FLOAT, "1", "seconds" },
    { "loop", CWED_ARG_STRING, "NONE", "NONE, LOOP or PING_PONG" },
    { 0 },
  };
  static const cwed_param track_p[] = {
    { "player", CWED_ARG_PATH, NULL, "the AnimationPlayer" },
    { "anim", CWED_ARG_STRING, NULL, "the animation" },
    { "kind", CWED_ARG_STRING, "value", "value, method or event" },
    { "path", CWED_ARG_STRING, "", "node:property (value), node (method), relative to the player's root_node" },
    { "interp", CWED_ARG_STRING, "LINEAR", "NEAREST, LINEAR, CUBIC or HERMITE (value tracks)" },
    { 0 },
  };
  static const cwed_param track_set_p[] = {
    { "player", CWED_ARG_PATH, NULL, "the AnimationPlayer" },
    { "anim", CWED_ARG_STRING, NULL, "the animation" },
    { "track", CWED_ARG_INT, NULL, "track index (0 = the first)" },
    { "interp", CWED_ARG_STRING, "LINEAR", "NEAREST, LINEAR, CUBIC or HERMITE" },
    { "rot", CWED_ARG_STRING, "SLERP", "quaternion tracks: SLERP or NLERP" },
    { "enabled", CWED_ARG_BOOL, "true", "false skips the track" },
    { 0 },
  };
  static const cwed_param key_p[] = {
    { "player", CWED_ARG_PATH, NULL, "the AnimationPlayer" },
    { "anim", CWED_ARG_STRING, NULL, "the animation" },
    { "track", CWED_ARG_INT, NULL, "track index (0 = the first)" },
    { "time", CWED_ARG_STRING, NULL, "seconds" },
    { "value", CWED_ARG_STRING, NULL, "the key as a scene file writes it (a value; `in=.. out=..` tangents on a HERMITE track)" },
    { 0 },
  };
  static const cwed_param text_p[] = {
    { "player", CWED_ARG_PATH, NULL, "the AnimationPlayer" },
    { "anim", CWED_ARG_STRING, NULL, "the animation" },
    { 0 },
  };
  cwed_command_register(&(cwed_command_desc){ .name = "anim.new", .title = "New Animation", .params = new_p, .flags = U,
    .doc = "Add an empty animation to an AnimationPlayer (stored in the scene).",
    .example = "anim.new player=Anim name=spin length=1.6 loop=LOOP", .exec = new_exec, .undo = undo, .free_state = free_state });
  cwed_command_register(&(cwed_command_desc){ .name = "anim.track_add", .title = "Add Animation Track", .params = track_p, .flags = U,
    .doc = "Add a track to an animation: a property (value), method calls (method) or events (event).",
    .example = "anim.track_add player=Anim anim=spin path=\"Spin:rotation\"", .exec = track_add_exec, .undo = undo,
    .free_state = free_state });
  cwed_command_register(&(cwed_command_desc){ .name = "anim.track_set", .title = "Set Animation Track", .params = track_set_p, .flags = U,
    .doc = "Change a track's interpolation, quaternion mode or enabled flag (only the arguments given).",
    .example = "anim.track_set player=Anim anim=spin track=0 interp=CUBIC", .exec = track_set_exec, .undo = undo,
    .free_state = free_state });
  cwed_command_register(&(cwed_command_desc){ .name = "anim.key_add", .title = "Add Animation Key", .params = key_p, .flags = U,
    .doc = "Add a key to a track, written as in a scene file.",
    .example = "anim.key_add player=Anim anim=spin track=0 time=0 value=\"vec3(0, 0, 0)\"", .exec = key_add_exec, .undo = undo,
    .free_state = free_state });
  cwed_command_register(&(cwed_command_desc){ .name = "anim.text", .title = "Animation Text", .params = text_p,
    .flags = CWED_CMD_SCENE | CWED_CMD_QUERY | CWED_CMD_HIDDEN, .doc = "An animation as scene-file text.", .exec = text_exec });
}

CWED_MODULE(animation) = { .title = "Animation", .init = init };

/* [animation] sections in .cwscene files, the debug-channel commands, and
 * registration of the animation node types. */
#include <stdio.h>
#include <stdlib.h>

#include "anim_internal.h"
#include "cartwheel/debug.h"
#include "cartwheel/scene_file.h"

typedef struct sec_state {
  cw_anim anim;
  const char *name, *player;
  int track;
} sec_state;

static int name_index(const char *s, const char *(*name)(int), int count) {
  for (int i = 0; i < count; i++)
    if (!strcmp(s, name(i))) return i;
  return -1;
}

static void *sec_begin(const char *h, const char *path, int lineno) {
  const char *name = cw_scene_attr(h, "name"), *len = cw_scene_attr(h, "length"), *loop = cw_scene_attr(h, "loop");
  if (!name || !len) { cw_error("%s:%d: [animation] needs name and length", path, lineno); return NULL; }
  int lm = loop ? name_index(loop, cw__anim_loop_name, 3) : 0;
  if (lm < 0) { cw_error("%s:%d: loop=%s: use NONE, LOOP or PING_PONG", path, lineno, loop); return NULL; }
  sec_state *st = cw_calloc(1, sizeof *st, CW_MEM_ANIM);
  st->name = name;
  st->player = cw_scene_attr(h, "player");
  if (!st->player) st->player = cw_intern(".");
  st->anim = cw_anim_new(name, strtof(len, NULL), (cw_loop_mode)lm);
  st->track = -1;
  return st;
}

/* end of one value expression: "text", vec3(1, 2), 3.5, NAME */
static const char *value_end(const char *s) {
  if (*s == '"') {
    for (s++; *s && *s != '"'; s++)
      if (*s == '\\' && s[1]) s++;
    return *s ? s + 1 : s;
  }
  int depth = 0;
  bool q = false;
  for (; *s; s++) {
    if (q) { if (*s == '\\' && s[1]) s++; else if (*s == '"') q = false; continue; }
    if (*s == '"') q = true;
    else if (*s == '(') depth++;
    else if (*s == ')') { if (--depth == 0) return s + 1; }
    else if ((*s == ' ' || *s == ',') && depth == 0) return s;
  }
  return s;
}

static bool parse_val(const char *s, const char *e, cw_value *out) {
  char buf[2048];
  size_t n = (size_t)(e - s);
  if (n >= sizeof buf) return false;
  memcpy(buf, s, n);
  buf[n] = 0;
  return n && cw_value_parse(buf, CW_T_NONE, out);
}

static const char *skip(const char *s) {
  while (*s == ' ' || *s == '\t') s++;
  return s;
}

static bool sec_line(void *vs, const char *line, const char *path, int lineno) {
  sec_state *st = vs;
  if (line[0] == '[') {
    const char *type = cw_scene_attr(line, "type"), *tp = cw_scene_attr(line, "path"), *in = cw_scene_attr(line, "interp"),
               *rot = cw_scene_attr(line, "rot"), *en = cw_scene_attr(line, "enabled");
    int kind = type ? name_index(type, cw__anim_kind_name, 3) : -1;
    if (kind < 0) { cw_error("%s:%d: [track] needs type=value|method|event", path, lineno); return false; }
    if (kind != CW_TRACK_EVENT && !tp) { cw_error("%s:%d: a %s track needs path=", path, lineno, type); return false; }
    int interp = in ? name_index(in, cw__anim_interp_name, 4) : CW_INTERP_LINEAR;
    if (interp < 0) { cw_error("%s:%d: interp=%s: use NEAREST, LINEAR, CUBIC or HERMITE", path, lineno, in); return false; }
    if (kind == CW_TRACK_VALUE) st->track = cw_anim_add_value_track(st->anim, tp, (cw_interp)interp);
    else if (kind == CW_TRACK_METHOD) st->track = cw_anim_add_method_track(st->anim, tp);
    else st->track = cw_anim_add_event_track(st->anim);
    if (rot && !strcmp(rot, "NLERP")) cw_anim_track_set_rot_mode(st->anim, st->track, CW_ROT_NLERP);
    if (en && !strcmp(en, "false")) cw_anim_track_set_enabled(st->anim, st->track, false);
    return true;
  }
  if (st->track < 0) { cw_error("%s:%d: keys come after a [track] header", path, lineno); return false; }
  const char *eq = strchr(line, '=');
  if (!eq) { cw_error("%s:%d: expected `time = value`", path, lineno); return false; }
  char *end;
  float time = strtof(line, &end);
  if (end == line) { cw_error("%s:%d: a key starts with its time", path, lineno); return false; }
  const char *v = skip(eq + 1);
  cw_track_kind kind = cw_anim_track_kind(st->anim, st->track);
  if (kind == CW_TRACK_VALUE) {
    const char *ve = value_end(v);
    cw_value val;
    if (!parse_val(v, ve, &val)) { cw_error("%s:%d: cannot parse value `%s`", path, lineno, v); return false; }
    const char *rest = skip(ve);
    if (!strncmp(rest, "in=", 3)) {
      const char *a = rest + 3, *ae = value_end(a), *o = skip(ae);
      cw_value tin, tout;
      if (strncmp(o, "out=", 4) || !parse_val(a, ae, &tin) || !parse_val(o + 4, value_end(o + 4), &tout)) {
        cw_error("%s:%d: hermite keys are `t = value in=tangent out=tangent`", path, lineno);
        return false;
      }
      cw_anim_add_key_hermite(st->anim, st->track, time, val, tin, tout);
    } else {
      cw_anim_add_key(st->anim, st->track, time, val);
    }
  } else if (kind == CW_TRACK_METHOD) {
    const char *lp = strchr(v, '(');
    if (!lp) { cw_error("%s:%d: method keys are `t = name(arg, ...)`", path, lineno); return false; }
    const char *mname = cw_intern_n(v, (size_t)(lp - v));
    cw_value args[CW_ANIM_MAX_ARGS];
    int argc = 0;
    const char *a = skip(lp + 1);
    while (*a && *a != ')') {
      if (argc == CW_ANIM_MAX_ARGS) { cw_error("%s:%d: at most %d arguments", path, lineno, CW_ANIM_MAX_ARGS); return false; }
      const char *ae = value_end(a);
      if (!parse_val(a, ae, &args[argc++])) { cw_error("%s:%d: cannot parse argument `%s`", path, lineno, a); return false; }
      a = skip(ae);
      if (*a == ',') a = skip(a + 1);
    }
    cw_anim_add_method_key(st->anim, st->track, time, mname, args, argc);
  } else {
    cw_value val;
    if (!parse_val(v, value_end(v), &val) || val.type != CW_T_STRING) {
      cw_error("%s:%d: event keys are `t = \"name\"`", path, lineno);
      return false;
    }
    cw_anim_add_event_key(st->anim, st->track, time, val.s);
  }
  return true;
}

static void sec_instance(void *vs, cw_node root, const char *path) {
  sec_state *st = vs;
  cw_node p = strcmp(st->player, ".") ? cw_node_get(root, st->player) : root;
  if (!cw_node_is(p, "AnimationPlayer")) {
    cw_error("%s: animation '%s': player '%s' is not an AnimationPlayer in this scene", path, st->name, st->player);
    return;
  }
  cw_anim_player_add(p, st->name, st->anim);
}

static void sec_free(void *vs) {
  sec_state *st = vs;
  cw_anim_release(st->anim);
  cw_free(st);
}

/* cw_anim_from_text: one [animation] section (cw_anim_to_text's output) parsed
 * as a scene file parses it; stored in scene files like a loaded one */
cw_anim cw_anim_from_text(const char *text) {
  sec_state *st = NULL;
  int lineno = 0;
  bool ok = true;
  for (const char *p = text; ok && p && *p;) {
    const char *nl = strchr(p, '\n');
    size_t n = nl ? (size_t)(nl - p) : strlen(p);
    char line[4096];
    lineno++;
    while (n && (p[n - 1] == '\r' || p[n - 1] == ' ' || p[n - 1] == '\t')) n--;
    const char *q = p;
    while (n && (*q == ' ' || *q == '\t')) q++, n--;
    p = nl ? nl + 1 : NULL;
    if (!n || *q == ';') continue;
    if (n >= sizeof line) { cw_error("cw_anim_from_text:%d: a line over %d bytes", lineno, (int)sizeof line); ok = false; break; }
    memcpy(line, q, n);
    line[n] = 0;
    if (!st) {
      if (strncmp(line, "[animation", 10) != 0) { cw_error("cw_anim_from_text:%d: expected an [animation] header", lineno); ok = false; break; }
      st = sec_begin(line, "cw_anim_from_text", lineno);
      ok = st != NULL;
    } else {
      ok = sec_line(st, line, "cw_anim_from_text", lineno);
    }
  }
  if (!st) {
    if (ok) cw_error("cw_anim_from_text: no [animation] header");
    return (cw_anim){ 0 };
  }
  cw_anim a = st->anim;
  if (ok) {
    cw_anim_set_stored(a, true);
    st->anim = (cw_anim){ 0 }; /* the caller's reference now */
  }
  sec_free(st);
  return ok ? a : (cw_anim){ 0 };
}

typedef struct { cw_node root; void (*put)(void *, const char *); void *ctx; } save_walk;

static bool save_fn(cw_node n, void *u) {
  save_walk *w = u;
  if (!cw_node_is(n, "AnimationPlayer") || cw_scene_node_in_instance(n, w->root)) return true;
  char path[1024];
  cw_node_path(n, w->root, path, sizeof path);
  for (int i = 0; i < cw_anim_player_count(n); i++) {
    const char *name = cw_anim_player_name_at(n, i);
    cw_anim a = cw_anim_player_get(n, name);
    anim_rec *r = cw__anim(a);
    if (!r || !r->stored) continue;
    char *text = cw__anim_to_text_named(a, name, path[0] ? path : ".");
    w->put(w->ctx, text);
    cw_free(text);
  }
  return true;
}

static void sec_save(cw_node root, void (*put)(void *, const char *), void *ctx) {
  save_walk w = { root, put, ctx };
  cw_node_walk(root, save_fn, &w);
}

static const char *const subs[] = { "track", NULL };
static const cw_scene_section anim_section = {
  .name = "animation",
  .subsections = subs,
  .begin = sec_begin,
  .line = sec_line,
  .instance = sec_instance,
  .free = sec_free,
  .save = sec_save,
};

void cw__register_anim_scene_section(void) { cw_scene_register_section(&anim_section); }

cw_node cw__anim_debug_node(const char *path, void *reply, const char *type) {
  cw_reply *r = reply;
  cw_tree *t = cw_current_tree();
  if (!t) { cw_reply_error(r, "no scene tree"); return (cw_node){ 0 }; }
  cw_node n = cw_node_get(cw_tree_root(t), path);
  if (!cw_node_valid(n)) { cw_reply_error(r, "no node at '%s' (paths look like /root/Level/Anim; try: tree)", path); return (cw_node){ 0 }; }
  if (type && !cw_node_is(n, type)) { cw_reply_error(r, "'%s' is a %s, not an %s", path, cw_node_get_type(n)->name, type); return (cw_node){ 0 }; }
  return n;
}

void cw__register_anim_types(void) {
  static bool done;
  if (done) return;
  done = true;
  cw__register_player_type();
  cw__register_tree_type();
  cw__register_ik_types();
  cw__register_anim_scene_section();
  cw__register_player_debug_commands();
  cw__register_tree_debug_commands();
}

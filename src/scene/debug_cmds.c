/* Debug-channel commands for the scene tree: what an agent (romdev's
 * wasm({op:'command'})), the editor (cartwheel/remote.h) or a test asks a
 * running game.
 *
 * Addressing a node (every command that takes <node>):
 *   #123            by uid: stable for the node's whole life, survives renames
 *                   and moves (what an editor's remote tree keys on)
 *   /root/Level/P   an absolute path
 *   Level/P         a path relative to the root
 * Replies name nodes with both "uid" and "path". */
#include <stdio.h>
#include <stdlib.h>

#include "scene_internal.h"
#include "cartwheel/debug.h"
#include "cartwheel/scene_file.h"

static cw_node resolve(const char *ref, cw_reply *r) {
  cw_tree *t = cw_current_tree();
  if (!t) {
    cw_reply_error(r, "no scene tree (this app does not run one: cw_scene_app or cw_set_current_tree)");
    return (cw_node){ 0 };
  }
  if (ref[0] == '#' || !strncmp(ref, "uid:", 4)) {
    const char *num = ref[0] == '#' ? ref + 1 : ref + 4;
    char *end;
    unsigned long long uid = strtoull(num, &end, 10);
    cw_node n = *num && !*end ? cw_node_by_uid(t, uid) : (cw_node){ 0 };
    if (CW_HANDLE_IS_NULL(n))
      cw_reply_error(r, "no node with uid %s in the tree (it was freed or never added; `tree` or `changes <revision>` list live uids)", num);
    return n;
  }
  cw_node n = cw_node_get(cw_tree_root(t), ref);
  if (CW_HANDLE_IS_NULL(n))
    cw_reply_error(r, "no node at '%s' (address nodes as #uid, /root/Level/Player or Level/Player; `tree` lists them)", ref);
  return n;
}

static void put_path(cw_reply *r, cw_node n) {
  char path[1024];
  cw_node_path(n, (cw_node){ 0 }, path, sizeof path);
  cw_reply_str(r, path);
}

static void node_json(cw_reply *r, cw_node n, int depth, int max_depth) {
  node_rec *rec = cw__node_rec(n);
  cw_reply_raw(r, "{\"name\":");
  cw_reply_str(r, rec->name);
  cw_reply_printf(r, ",\"type\":\"%s\",\"uid\":%llu", rec->type->name, (unsigned long long)rec->uid);
  if (rec->groups.count) {
    cw_reply_raw(r, ",\"groups\":[");
    for (int i = 0; i < rec->groups.count; i++) {
      if (i) cw_reply_raw(r, ",");
      cw_reply_str(r, rec->groups.data[i]);
    }
    cw_reply_raw(r, "]");
  }
  cw_node3d *d3 = cw_node_data(n, "Node3D");
  if (d3) cw_reply_printf(r, ",\"position\":[%g,%g,%g]", d3->position.x, d3->position.y, d3->position.z);
  cw_node2d *d2 = cw_node_data(n, "Node2D");
  if (d2) cw_reply_printf(r, ",\"position\":[%g,%g]", d2->position.x, d2->position.y);
  if (rec->children.count) {
    if (depth >= max_depth) {
      cw_reply_printf(r, ",\"children\":%d", rec->children.count);
    } else {
      cw_reply_raw(r, ",\"children\":[");
      for (int i = 0; i < rec->children.count; i++) {
        if (i) cw_reply_raw(r, ",");
        node_json(r, rec->children.data[i], depth + 1, max_depth);
      }
      cw_reply_raw(r, "]");
    }
  }
  if (depth > 0) cw_reply_raw(r, "}"); /* the caller closes the top object */
}

static uint64_t revision(cw_tree *t) { return cw__changes_enable(t)->rev - 1; }

static void cmd_tree(int argc, const char **argv, cw_reply *r) {
  cw_node n = resolve(argc > 1 ? argv[1] : "/root", r);
  if (CW_HANDLE_IS_NULL(n)) return;
  uint64_t rev = revision(cw_current_tree());
  node_json(r, n, 0, argc > 2 ? atoi(argv[2]) : 32);
  /* the revision this snapshot is at: `changes <revision>` continues from it */
  cw_reply_printf(r, ",\"revision\":%llu}", (unsigned long long)rev);
}

/* one node, flat: what a remote tree needs to place it */
static void cmd_node(int argc, const char **argv, cw_reply *r) {
  if (argc < 2) { cw_reply_error(r, "usage: node <node>   e.g. node #12 or node /root/Level/Player"); return; }
  cw_node n = resolve(argv[1], r);
  if (CW_HANDLE_IS_NULL(n)) return;
  node_rec *rec = cw__node_rec(n);
  node_rec *pr = cw__node_rec(rec->parent);
  cw_reply_raw(r, "{\"uid\":");
  cw_reply_printf(r, "%llu,\"name\":", (unsigned long long)rec->uid);
  cw_reply_str(r, rec->name);
  cw_reply_printf(r, ",\"type\":\"%s\",\"path\":", rec->type->name);
  put_path(r, n);
  cw_reply_printf(r, ",\"parent\":%llu,\"index\":%d,\"children\":[", (unsigned long long)(pr ? pr->uid : 0),
                  pr ? cw_node_index(n) : 0);
  for (int i = 0; i < rec->children.count; i++)
    cw_reply_printf(r, "%s%llu", i ? "," : "", (unsigned long long)cw_node_uid(rec->children.data[i]));
  cw_reply_printf(r, "],\"revision\":%llu}", (unsigned long long)revision(cw_current_tree()));
}

static void value_json(cw_reply *r, const cw_prop *p, const cw_value *v) {
  char buf[2048];
  if (p && p->type == CW_T_ENUM && p->enum_names) {
    int k = 0;
    while (p->enum_names[k] && k < v->i) k++;
    if (p->enum_names[k] && k == v->i) {
      cw_reply_str(r, p->enum_names[k]);
      return;
    }
  }
  cw_value_format(v, buf, sizeof buf);
  cw_reply_str(r, buf);
}

static const char *example(cw_type t) {
  switch (t) {
  case CW_T_BOOL: return "true";
  case CW_T_INT: return "3";
  case CW_T_FLOAT: return "2.5";
  case CW_T_VEC2: return "vec2(1, 2)";
  case CW_T_VEC3: return "vec3(1, 2, 3)";
  case CW_T_VEC4: return "vec4(1, 2, 3, 4)";
  case CW_T_QUAT: return "quat(0, 0, 0, 1)";
  case CW_T_COLOR: return "color(1, 0.5, 0, 1)";
  case CW_T_STRING: return "\"\\\"text\\\"\" (quote it)";
  case CW_T_NODE: return "node(\"/root/Level/Player\")";
  case CW_T_RES: return "res(0x1234)";
  default: return "?";
  }
}

/* Parse `text` as a value of type `want`, strictly: the whole text, the
 * right number of components. false with a message naming the fix. */
static bool parse_typed(const char *text, cw_type want, const char *const *enum_names, cw_value *out, char *err,
                        size_t errcap) {
  memset(out, 0, sizeof *out);
  if (want == CW_T_ENUM && enum_names) {
    for (int k = 0; enum_names[k]; k++)
      if (!strcmp(enum_names[k], text)) {
        out->type = CW_T_ENUM;
        out->i = k;
        return true;
      }
    char *end;
    long long i = strtoll(text, &end, 10);
    int count = 0;
    while (enum_names[count]) count++;
    if (*text && !*end && i >= 0 && i < count) {
      out->type = CW_T_ENUM;
      out->i = i;
      return true;
    }
    int n = snprintf(err, errcap, "wants one of: ");
    for (int k = 0; enum_names[k] && n < (int)errcap; k++) n += snprintf(err + n, errcap - (size_t)n, "%s%s", k ? ", " : "", enum_names[k]);
    return false;
  }
  if (want == CW_T_NODE && (text[0] == '#' || !strncmp(text, "uid:", 4))) {
    cw_tree *t = cw_current_tree();
    out->type = CW_T_NODE;
    out->node = t ? cw_node_by_uid(t, strtoull(text + (text[0] == '#' ? 1 : 4), NULL, 10)) : (cw_node){ 0 };
    if (CW_HANDLE_IS_NULL(out->node)) {
      snprintf(err, errcap, "no node %s (a node value is #uid or node(\"/root/path\"))", text);
      return false;
    }
    return true;
  }
  if (!cw_value_parse(text, want, out)) goto bad;
  /* the whole text: a number ends where strtod stopped, a vector at its ')' */
  {
    const char *end = text + strlen(text);
    while (end > text && (end[-1] == ' ' || end[-1] == '\t')) end--;
    if (out->type == CW_T_INT || out->type == CW_T_FLOAT) {
      char *stop;
      strtod(text, &stop);
      if (stop != end) goto bad;
    } else if (out->type >= CW_T_VEC2 && out->type <= CW_T_COLOR) {
      if (end == text || end[-1] != ')') goto bad;
      int commas = 0;
      for (const char *c = text; c < end; c++) commas += *c == ',';
      int need = out->type == CW_T_VEC2 ? 2 : out->type == CW_T_VEC3 ? 3 : 4;
      if (commas + 1 != need && !(out->type == CW_T_COLOR && commas + 1 == 3)) goto bad;
    }
  }
  /* the conversions cw_node_set makes (int -> float, vec3 -> color, ...) */
  {
    cw_type got = out->type;
    bool ok = got == want || want == CW_T_NONE || (want == CW_T_FLOAT && got == CW_T_INT) ||
              (want == CW_T_INT && got == CW_T_FLOAT && out->f == (double)(int64_t)out->f) ||
              (want == CW_T_BOOL && got == CW_T_INT) || (want == CW_T_COLOR && (got == CW_T_VEC3 || got == CW_T_VEC4)) ||
              ((want == CW_T_VEC3 || want == CW_T_VEC4) && got == CW_T_COLOR) || (want == CW_T_QUAT && got == CW_T_VEC4);
    if (!ok) goto bad;
  }
  return true;
bad:
  snprintf(err, errcap, "wants %s, written like %s", cw_type_name(want), example(want));
  return false;
}

static void cmd_get(int argc, const char **argv, cw_reply *r) {
  if (argc < 3) { cw_reply_error(r, "usage: get <node> <property> [property...]   e.g. get #12 position"); return; }
  cw_node n = resolve(argv[1], r);
  if (CW_HANDLE_IS_NULL(n)) return;
  for (int i = 2; i < argc; i++)
    if (!cw_node_find_prop(n, argv[i], NULL)) {
      cw_reply_error(r, "%s (%s) has no property '%s' (props %s lists them)", argv[1], cw_node_get_type(n)->name, argv[i], argv[1]);
      return;
    }
  if (argc == 3) {
    const cw_prop *p = cw_node_find_prop(n, argv[2], NULL);
    cw_value v;
    cw_node_get_prop(n, argv[2], &v);
    cw_reply_raw(r, "{\"value\":");
    value_json(r, p, &v);
    cw_reply_printf(r, ",\"type\":\"%s\"}", cw_type_name(p->type));
    return;
  }
  /* several at once: one round trip for an inspector's live values */
  cw_reply_printf(r, "{\"uid\":%llu,\"tick\":%llu,\"values\":{", (unsigned long long)cw_node_uid(n),
                  (unsigned long long)cw_tick_index());
  for (int i = 2; i < argc; i++) {
    const cw_prop *p = cw_node_find_prop(n, argv[i], NULL);
    cw_value v;
    cw_node_get_prop(n, argv[i], &v);
    cw_reply_printf(r, "%s\"%s\":{\"value\":", i > 2 ? "," : "", argv[i]);
    value_json(r, p, &v);
    cw_reply_printf(r, ",\"type\":\"%s\"}", cw_type_name(p->type));
  }
  cw_reply_raw(r, "}}");
}

static void cmd_set(int argc, const char **argv, cw_reply *r) {
  if (argc < 4) { cw_reply_error(r, "usage: set <node> <property> <value>   e.g. set #12 position vec3(0, 2, 0)"); return; }
  cw_node n = resolve(argv[1], r);
  if (CW_HANDLE_IS_NULL(n)) return;
  const cw_prop *p = cw_node_find_prop(n, argv[2], NULL);
  if (!p) { cw_reply_error(r, "%s (%s) has no property '%s' (props %s lists them)", argv[1], cw_node_get_type(n)->name, argv[2], argv[1]); return; }
  if (p->flags & CW_PROP_READONLY) { cw_reply_error(r, "%s.%s is read-only (computed by the engine; props %s marks these)", argv[1], argv[2], argv[1]); return; }
  /* a value with spaces arrives split unless quoted; join the rest back */
  char joined[2048];
  joined[0] = 0;
  for (int i = 3; i < argc; i++) {
    if (i > 3) strncat(joined, " ", sizeof joined - strlen(joined) - 1);
    strncat(joined, argv[i], sizeof joined - strlen(joined) - 1);
  }
  cw_value v;
  char err[512];
  if (!parse_typed(joined, p->type, p->enum_names, &v, err, sizeof err)) {
    cw_reply_error(r, "cannot set %s.%s to `%s`: it %s (e.g. set %s %s %s)", argv[1], argv[2], joined, err, argv[1], argv[2],
                   p->type == CW_T_ENUM && p->enum_names ? p->enum_names[0] : example(p->type));
    return;
  }
  if (!cw_node_set(n, argv[2], v)) {
    cw_reply_error(r, "cannot set %s.%s to `%s` (wants %s, e.g. %s)", argv[1], argv[2], joined, cw_type_name(p->type), example(p->type));
    return;
  }
  cw_tree *t = cw_current_tree();
  cw__changes_enable(t);
  cw__change_note(t, CW__CH_SET, n, -1, argv[2]);
  cw_node_get_prop(n, argv[2], &v);
  cw_reply_raw(r, "{\"value\":");
  value_json(r, p, &v);
  cw_reply_printf(r, ",\"type\":\"%s\",\"uid\":%llu,\"revision\":%llu}", cw_type_name(p->type),
                  (unsigned long long)cw_node_uid(n), (unsigned long long)revision(t));
}

static void cmd_props(int argc, const char **argv, cw_reply *r) {
  if (argc < 2) { cw_reply_error(r, "usage: props <node>   e.g. props #12"); return; }
  cw_node n = resolve(argv[1], r);
  if (CW_HANDLE_IS_NULL(n)) return;
  const cw_prop *props[256];
  const cw_node_type *owners[256];
  int np = cw_node_props(n, props, owners, 256);
  cw_reply_raw(r, "{\"props\":[");
  for (int i = 0; i < np && i < 256; i++) {
    if (i) cw_reply_raw(r, ",");
    cw_value v;
    cw_node_get_prop(n, props[i]->name, &v);
    cw_reply_printf(r, "{\"name\":\"%s\",\"type\":\"%s\",\"owner\":\"%s\",\"value\":", props[i]->name,
                    cw_type_name(props[i]->type), owners[i]->name);
    value_json(r, props[i], &v);
    if (props[i]->flags & CW_PROP_READONLY) cw_reply_raw(r, ",\"readonly\":true");
    if (props[i]->min != 0 || props[i]->max != 0) cw_reply_printf(r, ",\"min\":%g,\"max\":%g", props[i]->min, props[i]->max);
    if (props[i]->enum_names) {
      cw_reply_raw(r, ",\"enum\":[");
      for (int k = 0; props[i]->enum_names[k]; k++) {
        if (k) cw_reply_raw(r, ",");
        cw_reply_str(r, props[i]->enum_names[k]);
      }
      cw_reply_raw(r, "]");
    }
    if (props[i]->doc) {
      cw_reply_raw(r, ",\"doc\":");
      cw_reply_str(r, props[i]->doc);
    }
    cw_reply_raw(r, "}");
  }
  cw_reply_printf(r, "],\"uid\":%llu}", (unsigned long long)cw_node_uid(n));
}

static void cmd_methods(int argc, const char **argv, cw_reply *r) {
  if (argc < 2) { cw_reply_error(r, "usage: methods <node>   e.g. methods #12"); return; }
  cw_node n = resolve(argv[1], r);
  if (CW_HANDLE_IS_NULL(n)) return;
  const cw_method *ms[128];
  int k = cw_node_methods(n, ms, 128);
  cw_reply_raw(r, "{\"methods\":[");
  for (int i = 0; i < k && i < 128; i++) {
    cw_reply_printf(r, "%s{\"name\":\"%s\",\"type\":\"%s\",\"args\":", i ? "," : "", ms[i]->name, ms[i]->type);
    cw_reply_str(r, ms[i]->args ? ms[i]->args : "");
    if (ms[i]->doc) {
      cw_reply_raw(r, ",\"doc\":");
      cw_reply_str(r, ms[i]->doc);
    }
    cw_reply_raw(r, "}");
  }
  cw_reply_raw(r, "]}");
}

static cw_type type_by_name(const char *s, size_t n) {
  for (int t = CW_T_BOOL; t < CW_T_COUNT; t++)
    if (strlen(cw_type_name((cw_type)t)) == n && !strncmp(cw_type_name((cw_type)t), s, n)) return (cw_type)t;
  return CW_T_NONE;
}

static void cmd_call(int argc, const char **argv, cw_reply *r) {
  if (argc < 3) { cw_reply_error(r, "usage: call <node> <method> [args...]   e.g. call #12 boost 2.5 (methods <node> lists them)"); return; }
  cw_node n = resolve(argv[1], r);
  if (CW_HANDLE_IS_NULL(n)) return;
  const cw_method *m = cw_node_find_method(n, argv[2]);
  cw_value args[8];
  int k = 0;
  if (m && m->args && strcmp(m->args, "...")) { /* typed: count and types checked */
    const char *sig = m->args;
    int want = *sig ? 1 : 0;
    for (const char *c = sig; *c; c++) want += *c == ',';
    if (argc - 3 != want) {
      cw_reply_error(r, "%s takes %d argument%s (%s), got %d (e.g. call %s %s%s%s)", argv[2], want, want == 1 ? "" : "s",
                     *sig ? sig : "none", argc - 3, argv[1], argv[2], *sig ? " " : "", *sig ? example(type_by_name(sig, strcspn(sig, ","))) : "");
      return;
    }
    for (int i = 0; i < want && i < 8; i++) {
      size_t len = strcspn(sig, ",");
      cw_type ty = type_by_name(sig, len);
      char err[512];
      if (!parse_typed(argv[3 + i], ty, NULL, &args[k], err, sizeof err)) {
        cw_reply_error(r, "%s argument %d `%s` %s", argv[2], i + 1, argv[3 + i], err);
        return;
      }
      if (ty == CW_T_FLOAT && args[k].type == CW_T_INT) { args[k].f = (double)args[k].i; args[k].type = CW_T_FLOAT; }
      k++;
      sig += len + (sig[len] == ',');
    }
  } else {
    for (int i = 3; i < argc && k < 8; i++) {
      char err[512];
      if (!parse_typed(argv[i], CW_T_NONE, NULL, &args[k], err, sizeof err) && !cw_value_parse(argv[i], CW_T_NONE, &args[k])) {
        cw_reply_error(r, "argument %d `%s` is not a value (numbers, true/false, vec3(1, 2, 3), \"text\")", i - 2, argv[i]);
        return;
      }
      k++;
    }
  }
  cw_value ret = { CW_T_NONE };
  bool ok;
  if (m) {
    ok = m->fn(n, args, k, &ret, m->user);
  } else if (cw_node_call(n, argv[2], args, k, &ret)) { /* the method resolver answered */
    ok = true;
  } else {
    cw_reply_error(r, "%s (%s) has no method '%s' (methods %s lists them; a game adds one with cw_node_type_add_method)",
                   argv[1], cw_node_get_type(n)->name, argv[2], argv[1]);
    return;
  }
  if (!ok) {
    cw_reply_error(r, "%s.%s(...) failed (the method returned false: check its arguments with methods %s)", argv[1], argv[2], argv[1]);
    return;
  }
  cw_tree *t = cw_current_tree();
  cw__changes_enable(t);
  cw__change_note(t, CW__CH_CALLED, n, -1, argv[2]);
  cw_reply_raw(r, "{\"called\":");
  cw_reply_str(r, argv[2]);
  if (ret.type != CW_T_NONE) {
    cw_reply_raw(r, ",\"result\":");
    value_json(r, NULL, &ret);
    cw_reply_printf(r, ",\"result_type\":\"%s\"", cw_type_name(ret.type));
  }
  cw_reply_printf(r, ",\"tick\":%llu}", (unsigned long long)cw_tick_index());
}

static const char *kind_name[] = { "added", "removed", "renamed", "moved", "set", "called" };

static void cmd_changes(int argc, const char **argv, cw_reply *r) {
  cw_tree *t = cw_current_tree();
  if (!t) { cw_reply_error(r, "no scene tree (this app does not run one: cw_scene_app or cw_set_current_tree)"); return; }
  if (argc < 2) { cw_reply_error(r, "usage: changes <revision>: the revision comes from tree (or the last changes reply)"); return; }
  bool fresh = !t->changes;
  cw__changes *c = cw__changes_enable(t);
  uint64_t since = strtoull(argv[1], NULL, 10), now = c->rev - 1;
  int limit = argc > 2 ? atoi(argv[2]) : 2000;
  if (limit < 1) limit = 1;
  /* a client from before tracking began, or further back than the ring, or
   * from the future (the game restarted): it must take a new snapshot */
  if (fresh || since > now || now - since > CW__CHANGE_RING - 1) {
    cw_reply_printf(r, "{\"revision\":%llu,\"resync\":true,\"why\":\"%s\"}", (unsigned long long)now,
                    fresh ? "change tracking starts now: take a snapshot with tree"
                    : since > now ? "that revision is from another run of the game: take a new snapshot with tree"
                                  : "more changes than the log keeps: take a new snapshot with tree");
    return;
  }
  uint64_t upto = since + (uint64_t)limit < now ? since + (uint64_t)limit : now;
  cw_reply_printf(r, "{\"revision\":%llu,\"more\":%s,\"events\":[", (unsigned long long)upto, upto < now ? "true" : "false");
  for (uint64_t rev = since + 1; rev <= upto; rev++) {
    const cw__change *e = &c->ring[rev % CW__CHANGE_RING];
    cw_reply_printf(r, "%s{\"rev\":%llu,\"kind\":\"%s\",\"uid\":%llu", rev > since + 1 ? "," : "",
                    (unsigned long long)e->rev, kind_name[e->kind], (unsigned long long)e->uid);
    if (e->kind == CW__CH_ADDED || e->kind == CW__CH_MOVED)
      cw_reply_printf(r, ",\"parent\":%llu,\"index\":%d", (unsigned long long)e->parent, e->index);
    if (e->kind == CW__CH_ADDED || e->kind == CW__CH_RENAMED) {
      cw_reply_raw(r, ",\"name\":");
      cw_reply_str(r, e->name);
    }
    if (e->kind == CW__CH_ADDED) cw_reply_printf(r, ",\"type\":\"%s\"", e->type);
    if (e->prop) {
      cw_reply_raw(r, e->kind == CW__CH_CALLED ? ",\"method\":" : ",\"prop\":");
      cw_reply_str(r, e->prop);
    }
    cw_reply_raw(r, "}");
  }
  cw_reply_raw(r, "]}");
}

static void cmd_free(int argc, const char **argv, cw_reply *r) {
  if (argc < 2) { cw_reply_error(r, "usage: free <node>   e.g. free #12"); return; }
  cw_node n = resolve(argv[1], r);
  if (CW_HANDLE_IS_NULL(n)) return;
  if (n.id == cw_tree_root(cw_current_tree()).id) { cw_reply_error(r, "cannot free the root (change_scene replaces the scene under it)"); return; }
  uint64_t uid = cw_node_uid(n);
  cw_node_free(n);
  cw_reply_printf(r, "{\"freed\":true,\"uid\":%llu}", (unsigned long long)uid);
}

static void cmd_spawn(int argc, const char **argv, cw_reply *r) {
  if (argc < 3) { cw_reply_error(r, "usage: spawn <scene.cwscene | NodeType> <parent node> [name]   e.g. spawn OmniLight3D /root/Level Lamp"); return; }
  cw_node parent = resolve(argv[2], r);
  if (CW_HANDLE_IS_NULL(parent)) return;
  cw_node n = strstr(argv[1], ".cwscene") ? cw_scene_instance_path(argv[1]) : cw_node_new(argv[1], argc > 3 ? argv[3] : NULL);
  if (CW_HANDLE_IS_NULL(n)) { cw_reply_error(r, "could not create '%s' (a scene asset path or a registered type; `types` lists types)", argv[1]); return; }
  if (argc > 3) cw_node_set_name(n, argv[3]);
  cw_node_add_child(parent, n);
  cw_reply_raw(r, "{\"path\":");
  put_path(r, n);
  cw_reply_printf(r, ",\"uid\":%llu}", (unsigned long long)cw_node_uid(n));
}

static void cmd_save(int argc, const char **argv, cw_reply *r) {
  cw_node n = resolve(argc > 1 ? argv[1] : "/root", r);
  if (CW_HANDLE_IS_NULL(n)) return;
  char *text = cw_scene_save(n);
  cw_reply_raw(r, "{\"scene\":");
  cw_reply_str(r, text);
  cw_reply_raw(r, "}");
  cw_free(text);
}

/* the GAME's pause (a pause menu's): PAUSE_ALWAYS nodes keep running */
static void cmd_tree_pause(int argc, const char **argv, cw_reply *r) {
  cw_tree *t = cw_current_tree();
  if (!t) { cw_reply_error(r, "no scene tree (this app does not run one: cw_scene_app or cw_set_current_tree)"); return; }
  if (argc > 1) cw_tree_set_paused(t, atoi(argv[1]) != 0);
  cw_reply_printf(r, "{\"tree_paused\":%s,\"tree_tick\":%llu}", cw_tree_paused(t) ? "true" : "false", (unsigned long long)cw_tree_tick(t));
}

static void cmd_types(int argc, const char **argv, cw_reply *r) {
  (void)argc;
  (void)argv;
  cw_reply_raw(r, "{\"types\":[");
  for (int i = 0; i < cw_node_type_count(); i++) {
    const cw_node_type *t = cw_node_type_at(i);
    if (i) cw_reply_raw(r, ",");
    cw_reply_printf(r, "{\"name\":\"%s\",\"parent\":\"%s\"", t->name, t->parent ? t->parent : "");
    if (t->doc) {
      cw_reply_raw(r, ",\"doc\":");
      cw_reply_str(r, t->doc);
    }
    if (t->signals) {
      cw_reply_raw(r, ",\"signals\":[");
      for (int k = 0; t->signals[k]; k++) {
        if (k) cw_reply_raw(r, ",");
        cw_reply_str(r, t->signals[k]);
      }
      cw_reply_raw(r, "]");
    }
    cw_reply_raw(r, "}");
  }
  cw_reply_raw(r, "]}");
}

static void cmd_group(int argc, const char **argv, cw_reply *r) {
  if (argc < 2) { cw_reply_error(r, "usage: group <name>"); return; }
  cw_tree *t = cw_current_tree();
  if (!t) { cw_reply_error(r, "no scene tree (this app does not run one: cw_scene_app or cw_set_current_tree)"); return; }
  cw_node *list;
  int k = cw_tree_group(t, argv[1], &list);
  cw_reply_raw(r, "{\"nodes\":[");
  for (int i = 0; i < k; i++) {
    if (i) cw_reply_raw(r, ",");
    put_path(r, list[i]);
  }
  cw_reply_raw(r, "],\"uids\":[");
  for (int i = 0; i < k; i++) cw_reply_printf(r, "%s%llu", i ? "," : "", (unsigned long long)cw_node_uid(list[i]));
  cw_reply_raw(r, "]}");
}

static void cmd_emit(int argc, const char **argv, cw_reply *r) {
  if (argc < 3) { cw_reply_error(r, "usage: emit <node> <signal> [value...]   e.g. emit #12 hit 3"); return; }
  cw_node n = resolve(argv[1], r);
  if (CW_HANDLE_IS_NULL(n)) return;
  cw_value args[8];
  int k = 0;
  for (int i = 3; i < argc && k < 8; i++)
    if (cw_value_parse(argv[i], CW_T_NONE, &args[k])) k++;
  cw_emit(n, argv[2], args, k);
  cw_reply_raw(r, "{\"emitted\":true}");
}

void cw__scene_register_debug_commands(void) {
  cw__phys_register_debug_commands();
  cw_debug_command("tree", "tree [node] [depth]: the node tree as JSON (name, type, uid, groups, position) and the "
                           "revision it is at (changes <revision> continues from there)", cmd_tree);
  cw_debug_command("node", "node <node>: one node flat: uid, name, type, path, parent uid, index, child uids", cmd_node);
  cw_debug_command("changes", "changes <revision> [limit]: what changed since: added/removed/renamed/moved nodes and "
                              "debug-channel set/call edits, in order; resync:true means take a new tree", cmd_changes);
  cw_debug_command("get", "get <node> <property> [property...]: values (several in one reply)", cmd_get);
  cw_debug_command("set", "set <node> <property> <value>: set a property, type-checked (values as scene files write "
                          "them: 2.5, vec3(1, 2, 3), color(1, 0, 0, 1), \"text\", ENUM_NAME, #uid)", cmd_set);
  cw_debug_command("props", "props <node>: every property with type, value, owner type, range, enum names, doc", cmd_props);
  cw_debug_command("methods", "methods <node>: the methods call can invoke, with argument types", cmd_methods);
  cw_debug_command("call", "call <node> <method> [args...]: invoke a method (arguments type-checked against its signature)", cmd_call);
  cw_debug_command("free", "free <node>: free a node and its subtree", cmd_free);
  cw_debug_command("spawn", "spawn <scene.cwscene | NodeType> <parent node> [name]: add a node", cmd_spawn);
  cw_debug_command("save", "save [node]: the subtree as .cwscene text", cmd_save);
  cw_debug_command("tree_pause", "tree_pause [0|1]: query or set the GAME's pause (what a pause menu sets; "
                                 "the debugger's freeze is pause/step/resume)", cmd_tree_pause);
  cw_debug_command("types", "types: registered node types with docs and signals", cmd_types);
  cw_debug_command("group", "group <name>: paths and uids of the nodes in a group, tree order", cmd_group);
  cw_debug_command("emit", "emit <node> <signal> [args...]: emit a signal", cmd_emit);
}

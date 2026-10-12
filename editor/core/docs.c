/* Documents (open scenes) and the scene.* commands; the project. */
#include "internal.h"
#if !CW_CART
#include <dirent.h>
#include <sys/stat.h>
#endif

static CW_ARRAY(cwed_doc *) docs;
static cwed_doc *active;
static int untitled_seq;
static cw_tree *saved_current[8];
static int enter_depth;

/* ── accessors ────────────────────────────────────────────────────────── */

cwed_doc *cwed_doc_active(void) { return active; }
int cwed_doc_count(void) { return docs.count; }
cwed_doc *cwed_doc_at(int i) { return i >= 0 && i < docs.count ? docs.data[i] : NULL; }
int cwed_doc_index(cwed_doc *d) {
  for (int i = 0; i < docs.count; i++)
    if (docs.data[i] == d) return i;
  return -1;
}
cw_node cwed_doc_root(cwed_doc *d) { return d ? d->root : (cw_node){ 0 }; }
cw_tree *cwed_doc_tree(cwed_doc *d) { return d ? d->tree : NULL; }
const char *cwed_doc_path(cwed_doc *d) { return d ? d->path : ""; }
const char *cwed_doc_title(cwed_doc *d) { return d ? d->title : ""; }
bool cwed_doc_dirty(cwed_doc *d) { return d && cwed__history_marker(&d->hist) != d->saved_marker; }
uint32_t cwed_doc_version(cwed_doc *d) { return d ? d->version : 0; }

cw_node cwed_doc_node(cwed_doc *d, const char *path) {
  if (!d || !path) return (cw_node){ 0 };
  if (!*path || !strcmp(path, ".")) return d->root;
  if (path[0] == '/') return (cw_node){ 0 };
  return cw_node_get(d->root, path);
}

const char *cwed_doc_node_path(cwed_doc *d, cw_node n) {
  if (!d || !cw_node_valid(n)) return "";
  if (n.id == d->root.id) return ".";
  bool under = false;
  for (cw_node p = cw_node_parent(n); cw_node_valid(p); p = cw_node_parent(p))
    if (p.id == d->root.id) under = true;
  if (!under) return "";
  char buf[1024];
  cw_node_path(n, d->root, buf, sizeof buf);
  return cw_frame_printf("%s", buf);
}

void cwed_doc_enter(cwed_doc *d) {
  if (enter_depth < 8) saved_current[enter_depth] = cw_current_tree();
  enter_depth++;
  if (d) cw_set_current_tree(d->tree);
}
void cwed_doc_leave(void) {
  if (enter_depth <= 0) return;
  enter_depth--;
  if (enter_depth < 8) cw_set_current_tree(saved_current[enter_depth]);
}

char *cwed_doc_text(cwed_doc *d) {
  if (!d || !cw_node_valid(d->root)) return NULL;
  cwed_doc_enter(d);
  char *t = cw_scene_save(d->root);
  cwed_doc_leave();
  return t;
}

static int count_nodes(cw_node n) {
  int k = 1;
  for (int i = 0; i < cw_node_child_count(n); i++) k += count_nodes(cw_node_child(n, i));
  return k;
}

void cwed__doc_changed(cwed_doc *d, const char *line) {
  d->version++;
  char *t = cwed_doc_text(d);
  d->text_hash = t ? cw_hash32(t, strlen(t), CW_HASH32_INIT) : 0;
  cw_free(t);
  d->node_count = cw_node_valid(d->root) ? count_nodes(d->root) : 0;
  cwed__selection_prune(d);
  cwed_emit(&(cwed_event){ .type = CWED_EV_DOC_CHANGED, .doc = d, .line = line });
}

void cwed__doc_set_active(cwed_doc *d) {
  if (active == d) return;
  active = d;
  cwed_history_seal();
  cwed_emit(&(cwed_event){ .type = CWED_EV_DOC_ACTIVE, .doc = d });
  cwed_emit(&(cwed_event){ .type = CWED_EV_SELECTION, .doc = d });
  cwed_emit(&(cwed_event){ .type = CWED_EV_HISTORY, .doc = d });
}

static const char *base_name(const char *p) {
  const char *s = strrchr(p, '/');
  return s ? s + 1 : p;
}

/* tree: the tree root already sits in (scene.open loads straight into it), or NULL */
static cwed_doc *doc_new(cw_node root, const char *path, cw_tree *tree) {
  cwed_doc *d = cw_calloc(1, sizeof *d, CW_MEM_EDITOR);
  d->tree = tree ? tree : cw_tree_new();
  d->root = root;
  if (!tree) cw_node_add_child(cw_tree_root(d->tree), root);
  if (path && *path) {
    snprintf(d->path, sizeof d->path, "%s", path);
    snprintf(d->title, sizeof d->title, "%s", base_name(path));
  } else {
    snprintf(d->title, sizeof d->title, "untitled %d", ++untitled_seq);
  }
  cw_array_push(&docs, d);
  cwed__doc_changed(d, "open");
  cwed_emit(&(cwed_event){ .type = CWED_EV_DOC_OPENED, .doc = d });
  return d;
}

static void doc_free(cwed_doc *d) {
  cwed__history_free(&d->hist);
  cw_array_free(&d->sel);
  cw_tree_free(d->tree);
  cw_free(d);
}

static void doc_close(cwed_doc *d) {
  int i = cwed_doc_index(d);
  if (i < 0) return;
  memmove(docs.data + i, docs.data + i + 1, sizeof(cwed_doc *) * (size_t)(docs.count - i - 1));
  docs.count--;
  if (active == d) cwed__doc_set_active(docs.count ? docs.data[CW_MIN(i, docs.count - 1)] : NULL);
  cwed_emit(&(cwed_event){ .type = CWED_EV_DOC_CLOSED, .doc = d });
  doc_free(d);
}

void cwed__docs_shutdown(void) {
  while (docs.count) doc_close(docs.data[docs.count - 1]);
  cw_array_free(&docs);
}

static cwed_doc *doc_by_path(const char *path) {
  for (int i = 0; i < docs.count; i++)
    if (docs.data[i]->path[0] && !strcmp(docs.data[i]->path, path)) return docs.data[i];
  return NULL;
}

static bool valid_scene_path(const char *p) {
  size_t n = strlen(p);
  return n > 8 && !strcmp(p + n - 8, ".cwscene") && p[0] != '/' && !strstr(p, "..") && !strchr(p, '\\');
}

/* Types the scene names that this build cannot make: a scene with them
 * cannot round-trip, so it is refused instead of silently losing nodes. */
static int unknown_types_depth(const char *text, char *out, int cap, int depth) {
  int n = 0;
  for (const char *p = strstr(text, " type=\""); p; p = strstr(p + 1, " type=\"")) {
    const char *s = p + 7, *e = strchr(s, '"');
    if (!e) break;
    char name[128];
    snprintf(name, sizeof name, "%.*s", (int)(e - s), s);
    if (!cw_node_type_find(name) && !strstr(out, name)) {
      snprintf(out + strlen(out), (size_t)cap - strlen(out), "%s%s", out[0] ? ", " : "", name);
      n++;
    }
  }
  /* instanced sub-scenes too: an unknown type there loses the whole instance */
  for (const char *p = strstr(text, "[ext_scene "); p && depth < 8; p = strstr(p + 1, "[ext_scene ")) {
    const char *q = strstr(p, "path=\"");
    const char *nl = strchr(p, '\n');
    if (!q || (nl && q > nl)) continue;
    q += 6;
    const char *e = strchr(q, '"');
    if (!e) break;
    char sub[512];
    snprintf(sub, sizeof sub, "%.*s", (int)(e - q), q);
    size_t len;
    char *t = cw_vfs_read(sub, &len);
    if (t) {
      n += unknown_types_depth(t, out, cap, depth + 1);
      free(t);
    }
  }
  return n;
}

static int unknown_types(const char *text, char *out, int cap) {
  out[0] = 0;
  return unknown_types_depth(text, out, cap, 0);
}

/* first differing line (1-based) of two texts, 0 when equal */
static int first_diff_line(const char *a, const char *b) {
  int line = 1;
  for (; *a && *b && *a == *b; a++, b++)
    if (*a == '\n') line++;
  return *a == *b ? 0 : line;
}

/* Round-trip verdict of a load:
 *   canonical: saving it now reproduces the file byte for byte;
 *   lossless:  nothing was lost: the load logged no warning (an unknown
 *              property, a value that did not parse, an unresolved layer
 *              name), saving and loading the saved text again gives the
 *              same nodes, types, groups, instance sources and stored values,
 *              and the saved text is a fixed point (save(load(saved)) ==
 *              saved). A hand-written scene is usually lossless but not
 *              canonical (property order, enums by number, defaults written
 *              out); the editor saves the canonical form. */
typedef struct { cw_node root; bool lossless, canonical; int diff_line; char err[512], problem[512]; } load_result;

static int load_warnings;
static void count_warnings(cw_log_level level, const char *msg, void *user) {
  (void)msg, (void)user;
  if (level >= CW_LOG_WARN) __atomic_add_fetch(&load_warnings, 1, __ATOMIC_RELAXED);
}

static void prop_text(cw_node n, const cw_prop *p, char *out, int cap) {
  cw_value v;
  out[0] = 0;
  if (cw_node_get_prop(n, p->name, &v)) cw_value_format(&v, out, cap);
}

/* first difference between two subtrees, "" when equal */
static bool same_tree(cw_node a, cw_node b, cw_node ra, cw_node rb, char *why, int cap) {
  char pa[200], pb[200];
  cw_node_path(a, ra, pa, sizeof pa);
  cw_node_path(b, rb, pb, sizeof pb);
  if (strcmp(cw_node_name(a), cw_node_name(b)) || cw_node_get_type(a) != cw_node_get_type(b)) {
    snprintf(why, (size_t)cap, "node %s (%s) became %s (%s)", pa, cw_node_get_type(a)->name, pb, cw_node_get_type(b)->name);
    return false;
  }
  const char *sa = cw_scene_instance_source(a), *sb = cw_scene_instance_source(b);
  if ((sa || sb) && (!sa || !sb || strcmp(sa, sb))) {
    snprintf(why, (size_t)cap, "%s: instance source %s became %s", pa, sa ? sa : "none", sb ? sb : "none");
    return false;
  }
  const cw_prop *props[256];
  int k = cw_node_props(a, props, NULL, 256);
  for (int i = 0; i < k && i < 256; i++) {
    if (!(props[i]->flags & CW_PROP_STORED) || (props[i]->flags & CW_PROP_READONLY) || props[i]->type == CW_T_NODE) continue;
    char va[1024], vb[1024];
    prop_text(a, props[i], va, sizeof va);
    prop_text(b, props[i], vb, sizeof vb);
    if (strcmp(va, vb)) {
      snprintf(why, (size_t)cap, "%s.%s: %s became %s", pa[0] ? pa : ".", props[i]->name, va, vb);
      return false;
    }
  }
  if (cw_node_child_count(a) != cw_node_child_count(b)) {
    snprintf(why, (size_t)cap, "%s: %d children became %d", pa[0] ? pa : ".", cw_node_child_count(a), cw_node_child_count(b));
    return false;
  }
  for (int i = 0; i < cw_node_child_count(a); i++)
    if (!same_tree(cw_node_child(a, i), cw_node_child(b, i), ra, rb, why, cap)) return false;
  return true;
}

static cw_node instance_text(const char *text, const char *path) {
  cw_packed_scene *ps = cw_scene_parse(text, path);
  if (!ps) return (cw_node){ 0 };
  cw_node n = cw_scene_instance(ps);
  cw_packed_scene_free(ps);
  return n;
}

static load_result load_scene(const char *path, cw_tree *into, bool keep) {
  load_result r = { 0 };
  size_t len;
  char *text = cw_vfs_read(path, &len);
  if (!text) {
    snprintf(r.err, sizeof r.err, "no file '%s' in the project (try: project.info for the scenes it has)", path);
    return r;
  }
  char unknown[400];
  if (unknown_types(text, unknown, sizeof unknown)) {
    snprintf(r.err, sizeof r.err,
             "%s uses node types this editor build does not have: %s (they are registered by the game's C code; "
             "opening it would lose those nodes)", path, unknown);
    /* a type a project plugin provides: say which plugin and why it is not loaded */
    char first[128];
    snprintf(first, sizeof first, "%.*s", (int)strcspn(unknown, ","), unknown);
    const char *hint = cw_plugin_type_hint(first);
    if (hint) snprintf(r.err, sizeof r.err, "%s uses node type %s, which this editor does not have now: %s. The file is unchanged",
                       path, first, hint);
    free(text);
    return r;
  }
  cw_tree *prev = cw_current_tree();
  if (into) cw_set_current_tree(into);
  cw_scene_cache_forget(NULL); /* sub-scenes may have changed on disk or in the project layer */
  int lid = cw_log_add_listener(count_warnings, NULL);
  load_warnings = 0;
  r.root = instance_text(text, path);
  int warnings = load_warnings;
  if (CW_HANDLE_IS_NULL(r.root)) {
    snprintf(r.err, sizeof r.err, "%s does not load (the log has the line)", path);
  } else {
    if (into) cw_node_add_child(cw_tree_root(into), r.root);
    char *saved = cw_scene_save(r.root);
    r.diff_line = first_diff_line(saved, text);
    r.canonical = r.diff_line == 0;
    /* again from the saved text: same scene, same text */
    cw_node again = instance_text(saved, path);
    if (warnings) snprintf(r.problem, sizeof r.problem, "loading logged %d warning(s) (see the log): something was dropped", warnings);
    else if (CW_HANDLE_IS_NULL(again)) snprintf(r.problem, sizeof r.problem, "the saved text does not load again");
    else {
      /* compared detached: a sibling under the same root would be renamed */
      char why[512] = "";
      if (!same_tree(r.root, again, r.root, again, why, sizeof why)) snprintf(r.problem, sizeof r.problem, "%s", why);
      else {
        char *saved2 = cw_scene_save(again);
        if (strcmp(saved, saved2)) snprintf(r.problem, sizeof r.problem, "saving is not a fixed point (line %d)", first_diff_line(saved, saved2));
        cw_free(saved2);
      }
    }
    if (cw_node_valid(again)) cw_node_free(again);
    r.lossless = !r.problem[0];
    cw_free(saved);
    if (into && !keep) cw_node_remove_child(cw_tree_root(into), r.root);
  }
  cw_log_remove_listener(lid);
  cw_set_current_tree(prev);
  free(text);
  return r;
}

static void reply_roundtrip(cwed_call *c, const load_result *r) {
  cwed_json_printf(cwed_reply(c), ",\"lossless\":%s,\"canonical\":%s", r->lossless ? "true" : "false", r->canonical ? "true" : "false");
  if (!r->canonical) cwed_json_printf(cwed_reply(c), ",\"first_diff_line\":%d", r->diff_line);
  if (r->problem[0]) {
    cwed_json_raw(cwed_reply(c), ",\"problem\":");
    cwed_json_str(cwed_reply(c), r->problem);
  }
}

/* ── scene commands ───────────────────────────────────────────────────── */

static void reply_doc(cwed_call *c, cwed_doc *d) {
  cwed_json *j = cwed_reply(c);
  cwed_json_key(j, "doc");
  cwed_json_printf(j, "%d", cwed_doc_index(d));
  cwed_json_key(j, "path");
  cwed_json_str(j, d->path);
  cwed_json_key(j, "title");
  cwed_json_str(j, d->title);
  cwed_json_key(j, "nodes");
  cwed_json_printf(j, "%d", d->node_count);
}

static bool cmd_scene_new(cwed_call *c) {
  const char *type = cwed_arg(c, "type");
  const cw_node_type *t = cw_node_type_find(type);
  if (!t) return cwed_fail(c, "no node type '%s' (try: types over the debug channel, or Node3D / Node2D / Control)", type);
  cw_node root = cw_node_new(type, cwed_arg(c, "name"));
  cwed_doc *d = doc_new(root, NULL, NULL);
  cwed__doc_set_active(d);
  reply_doc(c, d);
  return true;
}

static bool cmd_scene_open(cwed_call *c) {
  const char *path = cwed_arg(c, "path");
  if (!valid_scene_path(path)) return cwed_fail(c, "'%s' is not a project scene path (relative, ending in .cwscene)", path);
  cwed_doc *d = doc_by_path(path);
  if (d) {
    cwed__doc_set_active(d);
    reply_doc(c, d);
    cwed_json_raw(cwed_reply(c), ",\"already_open\":true");
    return true;
  }
  cw_tree *t = cw_tree_new();
  load_result r = load_scene(path, t, true);
  if (CW_HANDLE_IS_NULL(r.root)) {
    cw_tree_free(t);
    return cwed_fail(c, "%s", r.err);
  }
  d = doc_new(r.root, path, t);
  cwed__doc_set_active(d);
  reply_doc(c, d);
  reply_roundtrip(c, &r);
  if (!r.lossless) cwed_log(CWED_LOG_WARN, "%s: saving it would lose something: %s", path, r.problem);
  else if (!r.canonical) cwed_log(CWED_LOG_INFO, "%s: will save in canonical form (from line %d)", path, r.diff_line);
  return true;
}

static bool write_doc(cwed_call *c, cwed_doc *d, const char *path_in) {
  char path[512]; /* path_in may be d->path itself */
  snprintf(path, sizeof path, "%s", path_in);
  char *text = cwed_doc_text(d);
  if (!text) return cwed_fail(c, "nothing to save");
  bool ok = cw_vfs_write(path, text, strlen(text));
  size_t bytes = strlen(text);
  cw_free(text);
  if (!ok) return cwed_fail(c, "could not write %s (see the log)", path);
  cw_scene_cache_forget(path);
  snprintf(d->path, sizeof d->path, "%s", path);
  snprintf(d->title, sizeof d->title, "%.159s", base_name(path));
  d->saved_marker = cwed__history_marker(&d->hist);
  cwed_json_key(cwed_reply(c), "saved");
  cwed_json_str(cwed_reply(c), path);
  cwed_json_key(cwed_reply(c), "bytes");
  cwed_json_printf(cwed_reply(c), "%zu", bytes);
  cwed_json_key(cwed_reply(c), "to_disk");
  cwed_json_raw(cwed_reply(c), cw_vfs_root() ? "true" : "false");
  cwed_emit(&(cwed_event){ .type = CWED_EV_DOC_SAVED, .doc = d });
  cwed_emit(&(cwed_event){ .type = CWED_EV_HISTORY, .doc = d });
  return true;
}

static bool cmd_scene_save(cwed_call *c) {
  cwed_doc *d = cwed_call_doc(c);
  const char *path = cwed_arg_given(c, "path") ? cwed_arg(c, "path") : d->path;
  if (!*path) return cwed_fail(c, "%s has no file yet: scene.save path=<name>.cwscene", d->title);
  if (!valid_scene_path(path)) return cwed_fail(c, "'%s' is not a project scene path (relative, ending in .cwscene)", path);
  cwed_doc *other = doc_by_path(path);
  if (other && other != d) return cwed_fail(c, "%s is open in another tab: close it first", path);
  return write_doc(c, d, path);
}

static bool cmd_scene_save_all(cwed_call *c) {
  int n = 0;
  for (int i = 0; i < docs.count; i++)
    if (docs.data[i]->path[0] && cwed_doc_dirty(docs.data[i])) {
      if (!write_doc(c, docs.data[i], docs.data[i]->path)) return false;
      n++;
    }
  cwed_json_key(cwed_reply(c), "count");
  cwed_json_printf(cwed_reply(c), "%d", n);
  return true;
}

static cwed_doc *doc_arg(cwed_call *c, const char *name) {
  const char *s = cwed_arg(c, name);
  if (!*s) return active;
  char *end;
  long i = strtol(s, &end, 10);
  if (!*end) return cwed_doc_at((int)i);
  return doc_by_path(s);
}

static bool cmd_scene_close(cwed_call *c) {
  cwed_doc *d = doc_arg(c, "doc");
  if (!d) return cwed_fail(c, "no such open scene (scene.list shows them)");
  if (cwed_doc_dirty(d) && !cwed_arg_bool(c, "force"))
    return cwed_fail(c, "%s has unsaved changes: scene.save first, or scene.close force=true", d->title);
  cwed_json_key(cwed_reply(c), "closed");
  cwed_json_str(cwed_reply(c), d->title);
  doc_close(d);
  return true;
}

static bool cmd_scene_switch(cwed_call *c) {
  cwed_doc *d = doc_arg(c, "doc");
  if (!d) return cwed_fail(c, "no such open scene (scene.list shows them)");
  cwed__doc_set_active(d);
  reply_doc(c, d);
  return true;
}

static bool cmd_scene_list(cwed_call *c) {
  cwed_json *j = cwed_reply(c);
  cwed_json_key(j, "docs");
  cwed_json_raw(j, "[");
  for (int i = 0; i < docs.count; i++) {
    cwed_doc *d = docs.data[i];
    cwed_json_printf(j, "%s{\"path\":", i ? "," : "");
    cwed_json_str(j, d->path);
    cwed_json_raw(j, ",\"title\":");
    cwed_json_str(j, d->title);
    cwed_json_printf(j, ",\"dirty\":%s,\"active\":%s,\"nodes\":%d,\"undo\":%d,\"redo\":%d}", cwed_doc_dirty(d) ? "true" : "false",
                     d == active ? "true" : "false", d->node_count, d->hist.pos, d->hist.entries.count - d->hist.pos);
  }
  cwed_json_raw(j, "]");
  return true;
}

static bool cmd_scene_text(cwed_call *c) {
  char *t = cwed_doc_text(cwed_call_doc(c));
  cwed_json_key(cwed_reply(c), "text");
  cwed_json_str(cwed_reply(c), t ? t : "");
  cw_free(t);
  return true;
}

static void tree_json(cwed_json *j, cwed_doc *d, cw_node n, int depth, int max) {
  cwed_json_raw(j, "{\"name\":");
  cwed_json_str(j, cw_node_name(n));
  cwed_json_raw(j, ",\"type\":");
  cwed_json_str(j, cw_node_get_type(n)->name);
  cwed_json_raw(j, ",\"path\":");
  cwed_json_str(j, cwed_doc_node_path(d, n));
  const char *src = cw_scene_instance_source(n);
  if (src) {
    cwed_json_raw(j, ",\"instance\":");
    cwed_json_str(j, src);
  }
  if (cwed_selection_has(n)) cwed_json_raw(j, ",\"selected\":true");
  int k = cw_node_child_count(n);
  if (k) {
    if (depth >= max) cwed_json_printf(j, ",\"children\":%d", k);
    else {
      cwed_json_raw(j, ",\"children\":[");
      for (int i = 0; i < k; i++) {
        if (i) cwed_json_raw(j, ",");
        tree_json(j, d, cw_node_child(n, i), depth + 1, max);
      }
      cwed_json_raw(j, "]");
    }
  }
  cwed_json_raw(j, "}");
}

static bool cmd_scene_tree(cwed_call *c) {
  cwed_doc *d = cwed_call_doc(c);
  cw_node n = cwed_arg_node(c, "path");
  if (CW_HANDLE_IS_NULL(n)) return false;
  cwed_json_key(cwed_reply(c), "tree");
  tree_json(cwed_reply(c), d, n, 0, (int)cwed_arg_int(c, "depth"));
  return true;
}

static bool cmd_scene_check(cwed_call *c) {
  const char *path = cwed_arg(c, "path");
  cw_tree *t = cw_tree_new();
  load_result r = load_scene(path, t, false);
  if (cw_node_valid(r.root)) cw_node_free(r.root);
  cw_tree_free(t);
  if (r.err[0] && CW_HANDLE_IS_NULL(r.root)) return cwed_fail(c, "%s", r.err);
  cwed_json_key(cwed_reply(c), "path");
  cwed_json_str(cwed_reply(c), path);
  reply_roundtrip(c, &r);
  return true;
}

void cwed__scene_commands(void) {
  static const cwed_param new_p[] = {
    { "type", CWED_ARG_STRING, "Node3D", "root node type" },
    { "name", CWED_ARG_STRING, "Scene", "root node name" },
    { 0 } };
  static const cwed_param path_p[] = { { "path", CWED_ARG_STRING, NULL, "project path of a .cwscene" }, { 0 } };
  static const cwed_param save_p[] = { { "path", CWED_ARG_STRING, "", "save as this project path (default: where it came from)" }, { 0 } };
  static const cwed_param close_p[] = {
    { "doc", CWED_ARG_STRING, "", "index or path of an open scene (default: the active one)" },
    { "force", CWED_ARG_BOOL, "false", "close even with unsaved changes" },
    { 0 } };
  static const cwed_param switch_p[] = { { "doc", CWED_ARG_STRING, NULL, "index or path of an open scene" }, { 0 } };
  static const cwed_param tree_p[] = {
    { "path", CWED_ARG_PATH, ".", "subtree root" },
    { "depth", CWED_ARG_INT, "64", "levels to expand" },
    { 0 } };
  cwed_command_register(&(cwed_command_desc){ .name = "scene.new", .title = "New Scene", .params = new_p, .shortcut = "ctrl+n",
    .doc = "Open a new, unsaved scene whose root is a node of `type`.", .exec = cmd_scene_new });
  cwed_command_register(&(cwed_command_desc){ .name = "scene.open", .title = "Open Scene...", .params = path_p, .shortcut = "ctrl+o",
    .doc = "Open a project .cwscene in a new tab (or switch to it). Replies lossless (saving loses nothing: no load "
           "warnings, the saved text loads back to the same scene and is a fixed point) and canonical (saving reproduces the "
           "file byte for byte).", .exec = cmd_scene_open });
  cwed_command_register(&(cwed_command_desc){ .name = "scene.save", .title = "Save Scene", .params = save_p, .shortcut = "ctrl+s",
    .flags = CWED_CMD_SCENE, .doc = "Write the active scene to the project (natively the project directory, in a cart the "
                                    "in-memory project layer: `vfs export` carries it out).", .exec = cmd_scene_save });
  cwed_command_register(&(cwed_command_desc){ .name = "scene.save_all", .title = "Save All Scenes", .shortcut = "ctrl+shift+s",
    .doc = "Save every open scene with unsaved changes that has a file.", .exec = cmd_scene_save_all });
  cwed_command_register(&(cwed_command_desc){ .name = "scene.close", .title = "Close Scene", .params = close_p, .shortcut = "ctrl+w",
    .doc = "Close an open scene (refused with unsaved changes unless force=true).", .exec = cmd_scene_close });
  cwed_command_register(&(cwed_command_desc){ .name = "scene.switch", .title = "Switch Scene", .params = switch_p,
    .flags = CWED_CMD_HIDDEN, .doc = "Make another open scene the active one.", .exec = cmd_scene_switch });
  cwed_command_register(&(cwed_command_desc){ .name = "scene.list", .title = "List Open Scenes", .flags = CWED_CMD_QUERY | CWED_CMD_HIDDEN,
    .doc = "The open scenes: path, title, dirty, active, node count, undo/redo depth.", .exec = cmd_scene_list });
  cwed_command_register(&(cwed_command_desc){ .name = "scene.text", .title = "Scene Text", .flags = CWED_CMD_SCENE | CWED_CMD_QUERY | CWED_CMD_HIDDEN,
    .doc = "The active scene as .cwscene text, as it would save now.", .exec = cmd_scene_text });
  cwed_command_register(&(cwed_command_desc){ .name = "scene.tree", .title = "Scene Tree", .params = tree_p,
    .flags = CWED_CMD_SCENE | CWED_CMD_QUERY | CWED_CMD_HIDDEN,
    .doc = "The active scene's nodes as JSON: name, type, path, instance source, selected, children.", .exec = cmd_scene_tree });
  cwed_command_register(&(cwed_command_desc){ .name = "scene.check", .title = "Check Scene Round Trip", .params = path_p,
    .flags = CWED_CMD_QUERY | CWED_CMD_HIDDEN,
    .doc = "Load a project scene without opening it and report lossless and canonical (as scene.open does), with the "
           "first problem found.",
    .exec = cmd_scene_check });
}

/* ── the project ──────────────────────────────────────────────────────── */

static char proj_name[128], proj_main[512], proj_scenes[2048], proj_dir[1024];

const char *cwed_project_name(void) { return proj_name; }
const char *cwed_project_main_scene(void) { return proj_main; }
const char *cwed_project_dir(void) { return proj_dir; }

/* every key = value line of project.cwproj (keys interned, values cw_alloc'd) */
static CW_ARRAY(struct proj_kv { const char *key; char *value; }) proj_kv;
const char *cwed_project_key(const char *key) {
  const char *k = cw_intern(key);
  for (int i = 0; i < proj_kv.count; i++)
    if (proj_kv.data[i].key == k) return proj_kv.data[i].value;
  return NULL;
}

static void trim(char *s) {
  char *e = s + strlen(s);
  while (e > s && (e[-1] == ' ' || e[-1] == '\t' || e[-1] == '\r' || e[-1] == '"')) *--e = 0;
  char *b = s;
  while (*b == ' ' || *b == '\t' || *b == '"') b++;
  memmove(s, b, strlen(b) + 1);
}

static void project_load(void) {
  proj_name[0] = proj_main[0] = proj_scenes[0] = 0;
  for (int i = 0; i < proj_kv.count; i++) cw_free(proj_kv.data[i].value);
  cw_array_clear(&proj_kv);
  const char *root = cw_vfs_root();
  snprintf(proj_dir, sizeof proj_dir, "%s", root ? root : "");
  size_t len;
  char *t = cw_vfs_read("project.cwproj", &len);
  if (t) {
    for (char *line = strtok(t, "\n"); line; line = strtok(NULL, "\n")) {
      char *eq = strchr(line, '=');
      if (!eq || line[0] == '#') continue;
      *eq = 0;
      char k[64], v[2048];
      snprintf(k, sizeof k, "%s", line);
      snprintf(v, sizeof v, "%s", eq + 1);
      trim(k);
      trim(v);
      char *dup = cw_alloc(strlen(v) + 1, CW_MEM_EDITOR);
      memcpy(dup, v, strlen(v) + 1);
      cw_array_push(&proj_kv, ((struct proj_kv){ cw_intern(k), dup }));
      if (!strcmp(k, "name")) snprintf(proj_name, sizeof proj_name, "%.127s", v);
      else if (!strcmp(k, "main_scene")) snprintf(proj_main, sizeof proj_main, "%.511s", v);
      else if (!strcmp(k, "scenes")) snprintf(proj_scenes, sizeof proj_scenes, "%s", v);
    }
    free(t);
  }
  if (!proj_name[0]) snprintf(proj_name, sizeof proj_name, "%s", root ? root : "untitled project");
  cwed_emit(&(cwed_event){ .type = CWED_EV_PROJECT });
}

void cwed_project_reload(void) { project_load(); }

typedef CW_ARRAY(const char *) str_list;
static void add_unique(str_list *l, const char *s) {
  s = cw_intern(s);
  for (int i = 0; i < l->count; i++)
    if (l->data[i] == s) return;
  cw_array_push(l, s);
}

#if !CW_CART
static void walk_dir(str_list *l, const char *root, const char *rel, int depth) {
  if (depth > 6 || l->count > 1000) return;
  char full[2048];
  snprintf(full, sizeof full, "%s%s%s", root, *rel ? "/" : "", rel);
  DIR *dir = opendir(full);
  if (!dir) return;
  struct dirent *e;
  while ((e = readdir(dir))) {
    if (e->d_name[0] == '.') continue;
    char sub[1024];
    snprintf(sub, sizeof sub, "%s%s%s", rel, *rel ? "/" : "", e->d_name);
    char path[2048];
    snprintf(path, sizeof path, "%s/%s", root, sub);
    struct stat st;
    if (stat(path, &st)) continue;
    if (S_ISDIR(st.st_mode)) walk_dir(l, root, sub, depth + 1);
    else if (valid_scene_path(sub)) add_unique(l, sub);
  }
  closedir(dir);
}
#endif

static int cmp_str(const void *a, const void *b) { return strcmp(*(const char *const *)a, *(const char *const *)b); }

int cwed__project_scenes(const char ***out) {
  static str_list l;
  cw_array_clear(&l);
  const char *s = proj_scenes;
  while (*s) {
    while (*s == ' ' || *s == ',') s++;
    const char *e = s;
    while (*e && *e != ',') e++;
    char p[512];
    snprintf(p, sizeof p, "%.*s", (int)(e - s), s);
    trim(p);
    if (valid_scene_path(p)) add_unique(&l, p);
    s = e;
  }
  if (proj_main[0]) add_unique(&l, proj_main);
#if !CW_CART
  if (cw_vfs_root()) walk_dir(&l, cw_vfs_root(), "", 0);
#endif
  const char **files;
  int n = cw_vfs_list(&files);
  for (int i = 0; i < n; i++)
    if (valid_scene_path(files[i])) add_unique(&l, files[i]);
  if (l.count > 1) qsort(l.data, (size_t)l.count, sizeof(char *), cmp_str);
  *out = l.data;
  return l.count;
}

static bool cmd_project_info(cwed_call *c) {
  cwed_json *j = cwed_reply(c);
  cwed_json_key(j, "name");
  cwed_json_str(j, proj_name);
  cwed_json_key(j, "dir");
  cwed_json_str(j, proj_dir);
  cwed_json_key(j, "main_scene");
  cwed_json_str(j, proj_main);
  cwed_json_key(j, "storage");
  cwed_json_str(j, CW_CART ? "memory (cart: vfs export carries changes out)" : proj_dir[0] ? "disk" : "memory (no project directory)");
  const char **sc;
  int n = cwed__project_scenes(&sc);
  cwed_json_key(j, "scenes");
  cwed_json_raw(j, "[");
  for (int i = 0; i < n; i++) {
    if (i) cwed_json_raw(j, ",");
    cwed_json_str(j, sc[i]);
  }
  cwed_json_raw(j, "]");
  cwed_json_key(j, "changed_files");
  cwed_json_printf(j, "%d", cw_vfs_changed_count());
  return true;
}

static bool cmd_project_open(cwed_call *c) {
#if CW_CART
  return cwed_fail(c, "a cart's project is its bundled files plus the in-memory layer: bring files in with `vfs import <archive>`"
                      "; project.open with a directory works in the native editor");
#else
  for (int i = 0; i < docs.count; i++)
    if (cwed_doc_dirty(docs.data[i]))
      return cwed_fail(c, "%s has unsaved changes: save or close it first", docs.data[i]->title);
  const char *dir = cwed_arg(c, "dir");
  struct stat st;
  if (stat(dir, &st) || !S_ISDIR(st.st_mode)) return cwed_fail(c, "'%s' is not a directory", dir);
  cwed__docs_shutdown();
  cw_vfs_set_root(dir);
  cw_asset_set_root(dir);
  project_load();
  if (proj_main[0] && cw_vfs_exists(proj_main)) cwed_runf("scene.open %s", cwed_quote(proj_main));
  return cmd_project_info(c);
#endif
}

void cwed__project_init(int argc, char **argv) {
#if !CW_CART
  if (argc > 1 && argv[1][0] != '-') cw_vfs_set_root(argv[1]);
  /* the project directory is also where its fonts, textures and themes load from */
  if (cw_vfs_root()) cw_asset_set_root(cw_vfs_root());
#else
  (void)argc, (void)argv;
#endif
  project_load();
  static const cwed_param open_p[] = { { "dir", CWED_ARG_STRING, NULL, "project directory (native editor)" }, { 0 } };
  cwed_command_register(&(cwed_command_desc){ .name = "project.open", .title = "Open Project...", .params = open_p,
    .doc = "Natively: make a directory the project (files read and written there) and open its main scene.",
    .exec = cmd_project_open });
  cwed_command_register(&(cwed_command_desc){ .name = "project.info", .title = "Project Info", .flags = CWED_CMD_QUERY,
    .doc = "The project: name, directory, main scene, the scenes it has, files changed in the project layer.",
    .exec = cmd_project_info });
}

void cwed__docs_init(void) { cwed__scene_commands(); }

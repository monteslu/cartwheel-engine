/* The command system: registry, command lines, history (undo/redo/merge),
 * and the debug-channel binding (`ed <line>`, `ed64 <base64 line>`). */
#include "internal.h"

/* ── JSON writer ──────────────────────────────────────────────────────── */

static void jput(cwed_json *j, const char *s, size_t n) {
  if (j->len + n + 1 > j->cap) {
    j->cap = (j->len + n + 1) * 2 + 128;
    j->buf = cw_realloc(j->buf, j->cap, CW_MEM_EDITOR);
  }
  memcpy(j->buf + j->len, s, n);
  j->len += n;
  j->buf[j->len] = 0;
}
void cwed_json_raw(cwed_json *j, const char *text) { jput(j, text, strlen(text)); }
void cwed_json_printf(cwed_json *j, const char *fmt, ...) {
  char tmp[1024];
  va_list ap;
  va_start(ap, fmt);
  int n = vsnprintf(tmp, sizeof tmp, fmt, ap);
  va_end(ap);
  if (n < (int)sizeof tmp) {
    jput(j, tmp, (size_t)n);
    return;
  }
  char *big = cw_alloc((size_t)n + 1, CW_MEM_EDITOR);
  va_start(ap, fmt);
  vsnprintf(big, (size_t)n + 1, fmt, ap);
  va_end(ap);
  jput(j, big, (size_t)n);
  cw_free(big);
}
void cwed_json_str(cwed_json *j, const char *s) {
  jput(j, "\"", 1);
  for (; s && *s; s++) {
    unsigned char c = (unsigned char)*s;
    if (c == '"' || c == '\\') {
      char e[2] = { '\\', (char)c };
      jput(j, e, 2);
    } else if (c == '\n') jput(j, "\\n", 2);
    else if (c == '\t') jput(j, "\\t", 2);
    else if (c < 32) {
      char e[8];
      snprintf(e, sizeof e, "\\u%04x", c);
      jput(j, e, 6);
    } else jput(j, (const char *)&c, 1);
  }
  jput(j, "\"", 1);
}
void cwed_json_key(cwed_json *j, const char *key) {
  size_t i = j->len;
  while (i > 0 && (j->buf[i - 1] == ' ' || j->buf[i - 1] == '\n')) i--;
  if (i > 0 && j->buf[i - 1] != '{' && j->buf[i - 1] != '[') jput(j, ",", 1);
  cwed_json_str(j, key);
  jput(j, ":", 1);
}
void cwed__json_reset(cwed_json *j) {
  j->len = 0;
  if (j->buf) j->buf[0] = 0;
}
void cwed__json_free(cwed_json *j) {
  cw_free(j->buf);
  *j = (cwed_json){ 0 };
}

/* ── command lines ────────────────────────────────────────────────────── */

int cwed__tokenize(const char *s, const char **tok, bool *quoted, int cap) {
  int n = 0;
  char buf[4096];
  while (*s && n < cap) {
    while (*s == ' ' || *s == '\t' || *s == '\n' || *s == '\r') s++;
    if (!*s) break;
    int k = 0;
    bool q = *s == '"'; /* a token that STARTS quoted is literal text (never key=value) */
    /* a token: runs of plain text, "quoted" parts and (paren groups) glued together */
    int depth = 0;
    while (*s && (depth > 0 || (*s != ' ' && *s != '\t' && *s != '\n' && *s != '\r'))) {
      if (*s == '"') {
        for (s++; *s && *s != '"'; s++) {
          if (*s == '\\' && (s[1] == '"' || s[1] == '\\')) s++;
          else if (*s == '\\' && s[1] == 'n') { s++; if (k < (int)sizeof buf - 1) buf[k++] = '\n'; continue; }
          if (k < (int)sizeof buf - 1) buf[k++] = *s;
        }
        if (*s == '"') s++;
        continue;
      }
      if (*s == '(') depth++;
      else if (*s == ')' && depth > 0) depth--;
      if (k < (int)sizeof buf - 1) buf[k++] = *s;
      s++;
    }
    buf[k] = 0;
    quoted[n] = q;
    tok[n++] = cw_intern(buf);
  }
  return n;
}

const char *cwed_quote(const char *a) {
  if (!a) a = "";
  bool need = !*a;
  for (const char *p = a; *p && !need; p++)
    if (*p == ' ' || *p == '\t' || *p == '"' || *p == '\\' || *p == '\n' || *p == '=' || *p == ';') need = true;
  if (!need) return cw_frame_printf("%s", a);
  size_t n = strlen(a);
  char *o = cw_frame_alloc(n * 2 + 3), *w = o;
  *w++ = '"';
  for (; *a; a++) {
    if (*a == '"' || *a == '\\') *w++ = '\\', *w++ = *a;
    else if (*a == '\n') *w++ = '\\', *w++ = 'n';
    else *w++ = *a;
  }
  *w++ = '"';
  *w = 0;
  return o;
}

/* ── registry ─────────────────────────────────────────────────────────── */

static CW_ARRAY(cwed_command_desc *) registry;
static CW_ARRAY(int) registry_owner; /* registration owner per entry (owners.c) */

void cwed_command_register(const cwed_command_desc *d) {
  CW_ASSERT(d && d->name && d->exec, "a command needs a name and exec");
  CW_ASSERT(!(d->flags & CWED_CMD_UNDO) || d->undo, "command %s is CWED_CMD_UNDO but has no undo", d->name);
  for (int i = 0; i < registry.count; i++)
    if (!strcmp(registry.data[i]->name, d->name)) {
      if (registry_owner.data[i] != cwed__owner && cwed__owner) {
        /* a plugin may not take over another owner's command: unloading it could not give the command back */
        cwed_log(CWED_LOG_ERROR, "plugin %s: command %s already exists (registered by %s); not replaced",
                 cwed_owner_name(cwed__owner), d->name, cwed_owner_name(registry_owner.data[i]));
        return;
      }
      *registry.data[i] = *d;
      return;
    }
  cwed_command_desc *c = cw_alloc(sizeof *c, CW_MEM_EDITOR);
  *c = *d;
  cw_array_push(&registry, c);
  cw_array_push(&registry_owner, cwed__owner);
}

void cwed__commands_owned(int owner, cwed_owner_stats *s) {
  for (int i = 0; i < registry.count; i++) s->commands += registry_owner.data[i] == owner;
}

void cwed__commands_remove_owner(int owner) {
  /* the descriptors stay allocated: history entries made by the plugin's
   * commands keep pointing at them (and at its code, which stays mapped) */
  int w = 0;
  for (int i = 0; i < registry.count; i++) {
    if (registry_owner.data[i] == owner) continue;
    registry.data[w] = registry.data[i];
    registry_owner.data[w++] = registry_owner.data[i];
  }
  registry.count = registry_owner.count = w;
}
const cwed_command_desc *cwed_command_find(const char *name) {
  for (int i = 0; i < registry.count; i++)
    if (!strcmp(registry.data[i]->name, name)) return registry.data[i];
  return NULL;
}
int cwed_command_count(void) { return registry.count; }
const cwed_command_desc *cwed_command_at(int i) { return i >= 0 && i < registry.count ? registry.data[i] : NULL; }

static int param_count(const cwed_command_desc *d) {
  int n = 0;
  while (d->params && d->params[n].name) n++;
  return n;
}

static int param_index(const cwed_command_desc *d, const char *name) {
  for (int i = 0; d->params && d->params[i].name; i++)
    if (!strcmp(d->params[i].name, name)) return i;
  return -1;
}

static const char *usage(const cwed_command_desc *d) {
  char *u = cw_frame_printf("%s", d->name);
  for (int i = 0; d->params && d->params[i].name; i++)
    u = cw_frame_printf("%s %s%s%s", u, d->params[i].def ? "[" : "<", d->params[i].name, d->params[i].def ? "]" : ">");
  return u;
}

/* ── calls ────────────────────────────────────────────────────────────── */

static uint64_t next_id = 1;
static int run_depth;
static uint32_t runs, errors;
static bool sealed = true;
uint32_t cwed__commands_run_count(void) { return runs; }
uint32_t cwed__commands_error_count(void) { return errors; }

const char *cwed_arg(cwed_call *c, const char *name) {
  int i = param_index(c->desc, name);
  if (i < 0) return "";
  if (c->vals[i]) return c->vals[i];
  return c->desc->params[i].def ? c->desc->params[i].def : "";
}
bool cwed_arg_given(cwed_call *c, const char *name) {
  int i = param_index(c->desc, name);
  return i >= 0 && c->vals[i];
}
int64_t cwed_arg_int(cwed_call *c, const char *name) { return strtoll(cwed_arg(c, name), NULL, 0); }
double cwed_arg_float(cwed_call *c, const char *name) { return strtod(cwed_arg(c, name), NULL); }
bool cwed_arg_bool(cwed_call *c, const char *name) {
  const char *s = cwed_arg(c, name);
  return !strcmp(s, "true") || !strcmp(s, "1") || !strcmp(s, "on") || !strcmp(s, "yes");
}

bool cwed_fail(cwed_call *c, const char *fmt, ...) {
  if (c->failed) return false; /* keep the first (most specific) reason */
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(c->error, sizeof c->error, fmt, ap);
  va_end(ap);
  c->failed = true;
  return false;
}

cw_node cwed_arg_node(cwed_call *c, const char *name) {
  const char *p = cwed_arg(c, name);
  if (!c->doc) {
    cwed_fail(c, "no scene is open (scene.open <path> or scene.new)");
    return (cw_node){ 0 };
  }
  cw_node n = cwed_doc_node(c->doc, p);
  if (CW_HANDLE_IS_NULL(n))
    cwed_fail(c, "no node '%s' in %s (paths are relative to the scene root, \".\" is the root; try: scene.tree)", p,
              cwed_doc_title(c->doc));
  return n;
}

bool cwed_arg_value(cwed_call *c, const char *name, cw_node n, const char *prop_name, cw_value *out) {
  const char *text = cwed_arg(c, name);
  const cw_prop *p = cw_node_find_prop(n, prop_name, NULL);
  if (!p) return cwed_fail(c, "%s (%s) has no property '%s' (try: node.props %s)", cw_node_name(n),
                           cw_node_get_type(n)->name, prop_name, cwed_doc_node_path(c->doc, n));
  memset(out, 0, sizeof *out);
  if (p->type == CW_T_ENUM && p->enum_names && ((text[0] >= 'A' && text[0] <= 'Z') || text[0] == '_')) {
    for (int k = 0; p->enum_names[k]; k++)
      if (!strcmp(p->enum_names[k], text)) {
        out->type = CW_T_ENUM;
        out->i = k;
        return true;
      }
    char names[512] = "";
    for (int k = 0; p->enum_names[k]; k++) snprintf(names + strlen(names), sizeof names - strlen(names), "%s%s", k ? " " : "", p->enum_names[k]);
    return cwed_fail(c, "'%s' is not a value of %s (one of: %s)", text, prop_name, names);
  }
  if (p->type == CW_T_STRING) {
    /* bare text is the string itself; a scene-file "quoted" form also works */
    if (text[0] == '"') return cw_value_parse(text, CW_T_STRING, out) ? true : cwed_fail(c, "bad string %s", text);
    *out = cw_val_string(text);
    return true;
  }
  if (p->type == CW_T_NODE) {
    /* node values are paths relative to the scene root */
    out->type = CW_T_NODE;
    if (!*text || !strcmp(text, "null")) return true;
    out->node = cwed_doc_node(c->doc, text);
    if (CW_HANDLE_IS_NULL(out->node)) return cwed_fail(c, "no node '%s' for %s", text, prop_name);
    return true;
  }
  if (!cw_value_parse(text, p->type, out))
    return cwed_fail(c, "cannot read `%s` as %s for %s (e.g. vec3(0, 1, 0), color(1, 0, 0, 1), 2.5, true)", text,
                     cw_type_name(p->type), prop_name);
  return true;
}

cwed_doc *cwed_call_doc(cwed_call *c) { return c->doc; }
void *cwed_call_state(cwed_call *c) { return c->state; }
void cwed_call_set_state(cwed_call *c, void *state) { c->state = state; }
cwed_json *cwed_reply(cwed_call *c) { return &c->reply; }
const char *cwed_call_line(cwed_call *c) { return c->line; }

static void build_line(cwed_call *c) {
  size_t cap = 64, len = 0;
  for (int i = 0; i < c->nargs; i++)
    if (c->vals[i]) cap += strlen(c->vals[i]) * 2 + strlen(c->desc->params[i].name) + 8;
  if (c->merge) cap += strlen(c->merge) * 2 + 16;
  char *s = cw_alloc(cap + strlen(c->desc->name), CW_MEM_EDITOR);
  len = (size_t)sprintf(s, "%s", c->desc->name);
  for (int i = 0; i < c->nargs; i++)
    if (c->vals[i]) len += (size_t)sprintf(s + len, " %s=%s", c->desc->params[i].name, cwed_quote(c->vals[i]));
  if (c->merge) len += (size_t)sprintf(s + len, " merge=%s", cwed_quote(c->merge));
  cw_free(c->line);
  c->line = s;
}

static void call_free(cwed_call *c) {
  if (!c) return;
  if (c->state) {
    if (c->desc->free_state) c->desc->free_state(c, c->state);
    else cw_free(c->state);
  }
  cw_free(c->line);
  cwed__json_free(&c->reply);
  cw_free(c);
}

/* Parse a line into a call. NULL + *err when it does not parse. */
static cwed_call *parse(const char *line, char *err, int cap) {
  const char *tok[CWED_MAX_ARGS + 8];
  bool quoted[CWED_MAX_ARGS + 8];
  int n = cwed__tokenize(line, tok, quoted, CWED_MAX_ARGS + 8);
  if (!n) {
    snprintf(err, (size_t)cap, "empty command (try: help)");
    return NULL;
  }
  const cwed_command_desc *d = cwed_command_find(tok[0]);
  if (!d) {
    snprintf(err, (size_t)cap, "unknown command '%s' (try: help, or help <prefix>)", tok[0]);
    return NULL;
  }
  cwed_call *c = cw_calloc(1, sizeof *c, CW_MEM_EDITOR);
  c->desc = d;
  c->nargs = param_count(d);
  int next_pos = 0;
  for (int i = 1; i < n; i++) {
    const char *t = tok[i];
    const char *eq = quoted[i] ? NULL : strchr(t, '=');
    int pi = -1;
    if (eq) {
      char key[64];
      size_t kl = (size_t)(eq - t);
      if (kl < sizeof key) {
        memcpy(key, t, kl);
        key[kl] = 0;
        if (!strcmp(key, "merge")) {
          c->merge = cw_intern(eq + 1);
          continue;
        }
        pi = param_index(d, key);
      }
      if (pi >= 0) t = cw_intern(eq + 1);
    }
    if (pi < 0) {
      while (next_pos < c->nargs && c->vals[next_pos]) next_pos++;
      if (next_pos >= c->nargs) {
        snprintf(err, (size_t)cap, "too many arguments for %s (usage: %s)", d->name, usage(d));
        call_free(c);
        return NULL;
      }
      pi = next_pos++;
    }
    if (c->vals[pi]) {
      snprintf(err, (size_t)cap, "%s given twice (usage: %s)", d->params[pi].name, usage(d));
      call_free(c);
      return NULL;
    }
    c->vals[pi] = t;
  }
  for (int i = 0; i < c->nargs; i++)
    if (!c->vals[i] && !d->params[i].def) {
      snprintf(err, (size_t)cap, "%s needs %s (usage: %s)", d->name, d->params[i].name, usage(d));
      call_free(c);
      return NULL;
    }
  build_line(c);
  return c;
}

/* ── history ──────────────────────────────────────────────────────────── */

static cwed_hist global_hist;
cwed_hist *cwed__global_history(void) { return &global_hist; }

uint64_t cwed__history_marker(cwed_hist *h) { return h->pos > 0 ? h->entries.data[h->pos - 1]->id : 0; }

static void clear_redo(cwed_hist *h) {
  /* newest first: a later entry may hold a node an earlier one also refers to */
  for (int i = h->entries.count - 1; i >= h->pos; i--) call_free(h->entries.data[i]);
  h->entries.count = h->pos;
}

void cwed__history_free(cwed_hist *h) {
  for (int i = h->entries.count - 1; i >= 0; i--) call_free(h->entries.data[i]);
  cw_array_free(&h->entries);
  h->pos = 0;
}

void cwed_history_seal(void) { sealed = true; }

static cwed_hist *hist_of(cwed_doc *d) { return d ? &d->hist : &global_hist; }

cwed_history_info cwed_history(cwed_doc *doc) {
  cwed_hist *h = hist_of(doc);
  cwed_history_info i = { h->pos, h->entries.count - h->pos, NULL, NULL };
  if (h->pos > 0) i.undo_line = h->entries.data[h->pos - 1]->line;
  if (h->pos < h->entries.count) i.redo_line = h->entries.data[h->pos]->line;
  return i;
}

const char *cwed_history_line(cwed_doc *doc, int i, bool *is_done) {
  cwed_hist *h = hist_of(doc);
  if (i < 0 || i >= h->entries.count) return NULL;
  if (is_done) *is_done = i < h->pos;
  return h->entries.data[i]->line;
}

bool cwed__undo(cwed_hist *h, cwed_doc *doc, char *err, int cap) {
  if (h->pos == 0) {
    snprintf(err, (size_t)cap, "nothing to undo in %s", doc ? cwed_doc_title(doc) : "the editor history");
    return false;
  }
  cwed_call *c = h->entries.data[--h->pos];
  if (doc) cwed_doc_enter(doc);
  c->desc->undo(c);
  if (doc) cwed_doc_leave();
  sealed = true;
  cwed_log(CWED_LOG_COMMAND, "undo: %s", c->line);
  if (doc) cwed__doc_changed(doc, c->line);
  cwed_emit(&(cwed_event){ .type = CWED_EV_HISTORY, .doc = doc });
  return true;
}

bool cwed__redo(cwed_hist *h, cwed_doc *doc, char *err, int cap) {
  if (h->pos >= h->entries.count) {
    snprintf(err, (size_t)cap, "nothing to redo in %s", doc ? cwed_doc_title(doc) : "the editor history");
    return false;
  }
  cwed_call *c = h->entries.data[h->pos];
  cwed__json_reset(&c->reply);
  c->failed = false;
  if (doc) cwed_doc_enter(doc);
  run_depth++;
  bool ok = c->desc->exec(c);
  run_depth--;
  if (doc) cwed_doc_leave();
  if (!ok) {
    snprintf(err, (size_t)cap, "redo of `%s` failed: %s", c->line, c->error);
    return false;
  }
  h->pos++;
  sealed = true;
  cwed_log(CWED_LOG_COMMAND, "redo: %s", c->line);
  if (doc) cwed__doc_changed(doc, c->line);
  cwed_emit(&(cwed_event){ .type = CWED_EV_HISTORY, .doc = doc });
  return true;
}

/* ── run ──────────────────────────────────────────────────────────────── */

static char *reply_text(cwed_call *c, bool ok, const char *err) {
  cwed_json j = { 0 };
  if (ok) {
    cwed_json_raw(&j, "{\"ok\":true");
    if (c && c->reply.len) {
      cwed_json_raw(&j, ",");
      cwed_json_raw(&j, c->reply.buf);
    }
    cwed_json_raw(&j, "}");
  } else {
    cwed_json_raw(&j, "{\"ok\":false,\"error\":");
    cwed_json_str(&j, err);
    cwed_json_raw(&j, "}");
  }
  return j.buf;
}

bool cwed_run(const char *line, char **json_out) {
  char err[640];
  cwed_call *c = parse(line, err, sizeof err);
  runs++;
  if (!c) {
    errors++;
    cwed_log(CWED_LOG_ERROR, "%s: %s", line, err);
    if (json_out) *json_out = reply_text(NULL, false, err);
    cwed_emit(&(cwed_event){ .type = CWED_EV_COMMAND, .line = line, .ok = false });
    return false;
  }
  const cwed_command_desc *d = c->desc;
  if (d->flags & CWED_CMD_SCENE) {
    c->doc = cwed_doc_active();
    if (!c->doc) {
      cwed_fail(c, "%s needs an open scene (scene.open <path> or scene.new)", d->name);
      goto done;
    }
  }
  if (c->doc) cwed_doc_enter(c->doc);
  run_depth++;
  bool ok = d->exec(c);
  run_depth--;
  if (c->doc) cwed_doc_leave();
  if (!ok && !c->failed) cwed_fail(c, "%s refused (no reason given)", d->name);
done:;
  bool success = !c->failed;
  char *reply = reply_text(c, success, c->error);
  if (!success) {
    errors++;
    cwed_log(CWED_LOG_ERROR, "%s: %s", c->line, c->error);
  } else if (!(d->flags & CWED_CMD_QUERY) && run_depth == 0) {
    cwed_log(CWED_LOG_COMMAND, "%s", c->line);
  }
  const char *line_copy = cw_frame_printf("%s", c->line);
  cwed_doc *doc = c->doc;
  bool recorded = false, has_merge = c->merge != NULL;
  if (success && (d->flags & CWED_CMD_UNDO) && run_depth == 0) {
    cwed_hist *h = hist_of(doc);
    clear_redo(h);
    cwed_call *top = h->pos > 0 ? h->entries.data[h->pos - 1] : NULL;
    if (c->merge && !sealed && top && top->desc == d && top->merge == c->merge && d->merge && d->merge(top, c)) {
      /* the top entry now stands for both: it redoes with the newest args */
      memcpy(top->vals, c->vals, sizeof top->vals);
      cw_free(top->line);
      top->line = c->line;
      c->line = NULL;
      top->id = next_id++;
      call_free(c);
    } else {
      c->id = next_id++;
      cw_array_push(&h->entries, c);
      h->pos = h->entries.count;
    }
    recorded = true;
    sealed = !has_merge;
  } else if (success && !(d->flags & CWED_CMD_QUERY) && run_depth == 0) {
    sealed = true; /* any other action ends a merge run */
  }
  if (success && (d->flags & CWED_CMD_UNDO) && run_depth == 0) {
    if (doc) cwed__doc_changed(doc, line_copy);
    cwed_emit(&(cwed_event){ .type = CWED_EV_HISTORY, .doc = doc });
  }
  if (!recorded) call_free(c);
  if (json_out) *json_out = reply;
  else cw_free(reply);
  cwed_emit(&(cwed_event){ .type = CWED_EV_COMMAND, .line = line_copy, .ok = success });
  return success;
}

bool cwed_runf(const char *fmt, ...) {
  char buf[4096];
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(buf, sizeof buf, fmt, ap);
  va_end(ap);
  return cwed_run(buf, NULL);
}

/* ── the debug channel ────────────────────────────────────────────────── */

static void reply_json(cw_reply *r, char *json) {
  cw_reply_raw(r, json);
  cw_free(json);
}

static void dbg_ed(int argc, const char **argv, cw_reply *r) {
  if (argc < 2) {
    cw_reply_error(r, "usage: ed <command line>  (ed help lists the editor commands)");
    return;
  }
  /* the channel split the request already: join it back, re-quoting what it unquoted */
  size_t cap = 16;
  for (int i = 1; i < argc; i++) cap += strlen(argv[i]) * 2 + 4;
  char *line = cw_alloc(cap, CW_MEM_EDITOR);
  size_t len = 0;
  line[0] = 0;
  for (int i = 1; i < argc; i++) {
    const char *a = argv[i];
    const char *eq = strchr(a, '=');
    bool keyed = eq && eq > a;
    for (const char *k = a; keyed && k < eq; k++)
      if (!((*k >= 'a' && *k <= 'z') || (*k >= '0' && *k <= '9') || *k == '_')) keyed = false;
    if (i > 1) line[len++] = ' ';
    if (i == 1) len += (size_t)sprintf(line + len, "%s", a);
    else if (keyed) len += (size_t)sprintf(line + len, "%.*s=%s", (int)(eq - a), a, cwed_quote(eq + 1));
    else len += (size_t)sprintf(line + len, "%s", cwed_quote(a));
  }
  char *json;
  cwed_run(line, &json);
  cw_free(line);
  reply_json(r, json);
}

static void dbg_ed64(int argc, const char **argv, cw_reply *r) {
  if (argc < 2) {
    cw_reply_error(r, "usage: ed64 <base64 of a command line>");
    return;
  }
  size_t n;
  char *raw = cw_base64_decode(argv[1], &n);
  if (!raw) {
    cw_reply_error(r, "ed64: bad base64");
    return;
  }
  char *line = cw_alloc(n + 1, CW_MEM_EDITOR);
  memcpy(line, raw, n);
  line[n] = 0;
  cw_free(raw);
  char *json;
  cwed_run(line, &json);
  cw_free(line);
  reply_json(r, json);
}

void cwed__commands_init(void) {
  cw_debug_command("ed", "ed <command line>: run an editor command (ed help lists them; replies are JSON)", dbg_ed);
  cw_debug_command("ed64", "ed64 <base64>: run a base64-encoded editor command line (quotes, newlines)", dbg_ed64);
}

/* Events, the editor log and the shared selection model. */
#include "internal.h"

/* ── events ───────────────────────────────────────────────────────────── */

typedef struct { uint32_t mask; cwed_event_fn fn; void *user; int owner; } sub;
static CW_ARRAY(sub) subs;

void cwed_subscribe(uint32_t mask, cwed_event_fn fn, void *user) { cw_array_push(&subs, ((sub){ mask, fn, user, cwed__owner })); }

void cwed__events_owned(int owner, cwed_owner_stats *s) {
  for (int i = 0; i < subs.count; i++) s->subscriptions += subs.data[i].owner == owner;
}
void cwed__events_remove_owner(int owner) {
  int w = 0;
  for (int i = 0; i < subs.count; i++)
    if (subs.data[i].owner != owner) subs.data[w++] = subs.data[i];
  subs.count = w;
}

void cwed_emit(const cwed_event *e) {
  static int depth;
  if (depth > 8) return; /* a subscriber that emits what it listens to: stop the loop */
  depth++;
  for (int i = 0; i < subs.count; i++)
    if (subs.data[i].mask & (uint32_t)e->type) subs.data[i].fn(e, subs.data[i].user);
  depth--;
}

/* ── log ──────────────────────────────────────────────────────────────── */

#define LOG_KEEP 2000
typedef struct { char *text; cwed_log_kind kind; } log_line;
static log_line ring[LOG_KEEP];
static int log_head, log_total; /* log_total = lines ever logged */
static bool in_log;

void cwed_log(cwed_log_kind kind, const char *fmt, ...) {
  char msg[1024];
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(msg, sizeof msg, fmt, ap);
  va_end(ap);
  log_line *l = &ring[log_head];
  cw_free(l->text);
  l->text = cw_strdup(msg, CW_MEM_EDITOR);
  l->kind = kind;
  log_head = (log_head + 1) % LOG_KEEP;
  log_total++;
  if (in_log) return;
  in_log = true;
  cwed_emit(&(cwed_event){ .type = CWED_EV_LOG, .line = l->text });
  in_log = false;
}

int cwed_log_count(void) { return log_total < LOG_KEEP ? log_total : LOG_KEEP; }

const char *cwed_log_line(int i, cwed_log_kind *kind) {
  int n = cwed_log_count();
  if (i < 0 || i >= n) return NULL;
  int start = log_total < LOG_KEEP ? 0 : log_head;
  log_line *l = &ring[(start + i) % LOG_KEEP];
  if (kind) *kind = l->kind;
  return l->text;
}

/* engine log lines (warnings and errors only: info is chatty) reach the
 * editor log through a listener; they may come from worker threads, so they
 * queue under a spin lock and drain on the main thread each frame */
#define PEND 64
static char pend_text[PEND][256];
static int pend_kind[PEND], pend_count;
static int pend_lock;

static void engine_log(cw_log_level level, const char *msg, void *user) {
  (void)user;
  if (level < CW_LOG_WARN) return;
  while (__atomic_exchange_n(&pend_lock, 1, __ATOMIC_ACQUIRE)) {
  }
  if (pend_count < PEND) {
    snprintf(pend_text[pend_count], sizeof pend_text[0], "%s", msg);
    pend_kind[pend_count++] = level == CW_LOG_WARN ? CWED_LOG_WARN : CWED_LOG_ERROR;
  }
  __atomic_store_n(&pend_lock, 0, __ATOMIC_RELEASE);
}

void cwed__log_drain(void);
void cwed__log_drain(void) {
  char text[PEND][256];
  int kind[PEND], n;
  while (__atomic_exchange_n(&pend_lock, 1, __ATOMIC_ACQUIRE)) {
  }
  n = pend_count;
  memcpy(text, pend_text, sizeof(pend_text[0]) * (size_t)n);
  memcpy(kind, pend_kind, sizeof(int) * (size_t)n);
  pend_count = 0;
  __atomic_store_n(&pend_lock, 0, __ATOMIC_RELEASE);
  for (int i = 0; i < n; i++) cwed_log((cwed_log_kind)kind[i], "engine: %s", text[i]);
}

void cwed__log_init(void) { cw_log_add_listener(engine_log, NULL); }

/* ── selection ────────────────────────────────────────────────────────── */

static bool node_in_doc(cwed_doc *d, cw_node n) {
  for (cw_node p = n; cw_node_valid(p); p = cw_node_parent(p))
    if (p.id == d->root.id) return true;
  return false;
}

void cwed__selection_prune(cwed_doc *d) {
  if (!d) return;
  int k = 0;
  for (int i = 0; i < d->sel.count; i++)
    if (node_in_doc(d, d->sel.data[i])) d->sel.data[k++] = d->sel.data[i];
  d->sel.count = k;
}

int cwed_selection_count(void) {
  cwed_doc *d = cwed_doc_active();
  if (!d) return 0;
  cwed__selection_prune(d);
  return d->sel.count;
}
cw_node cwed_selection_get(int i) {
  cwed_doc *d = cwed_doc_active();
  return d && i >= 0 && i < d->sel.count ? d->sel.data[i] : (cw_node){ 0 };
}
bool cwed_selection_has(cw_node n) {
  cwed_doc *d = cwed_doc_active();
  for (int i = 0; d && i < d->sel.count; i++)
    if (d->sel.data[i].id == n.id) return true;
  return false;
}
void cwed_selection_set(const cw_node *nodes, int count) {
  cwed_doc *d = cwed_doc_active();
  if (!d) return;
  bool same = count == d->sel.count;
  for (int i = 0; same && i < count; i++) same = nodes[i].id == d->sel.data[i].id;
  if (same) return;
  cw_array_clear(&d->sel);
  for (int i = 0; i < count; i++)
    if (node_in_doc(d, nodes[i]) && !cwed_selection_has(nodes[i])) cw_array_push(&d->sel, nodes[i]);
  cwed_emit(&(cwed_event){ .type = CWED_EV_SELECTION, .doc = d });
}

uint32_t cwed__selection_hash(void) {
  cwed_doc *d = cwed_doc_active();
  uint32_t h = CW_HASH32_INIT;
  for (int i = 0; d && i < d->sel.count; i++) {
    const char *p = cwed_doc_node_path(d, d->sel.data[i]);
    h = cw_hash32(p, strlen(p) + 1, h);
  }
  return h;
}

/* select paths=a,b mode=set|add|remove|toggle */
static bool cmd_select(cwed_call *c) {
  cwed_doc *d = cwed_call_doc(c);
  const char *mode = cwed_arg(c, "mode");
  if (strcmp(mode, "set") && strcmp(mode, "add") && strcmp(mode, "remove") && strcmp(mode, "toggle"))
    return cwed_fail(c, "mode is set, add, remove or toggle (got '%s')", mode);
  CW_ARRAY(cw_node) out = { 0 };
  if (strcmp(mode, "set"))
    for (int i = 0; i < d->sel.count; i++) cw_array_push(&out, d->sel.data[i]);
  const char *p = cwed_arg(c, "paths");
  while (*p) {
    const char *e = strchr(p, ',');
    size_t l = e ? (size_t)(e - p) : strlen(p);
    char path[512];
    snprintf(path, sizeof path, "%.*s", (int)l, p);
    if (path[0]) {
      cw_node n = cwed_doc_node(d, path);
      if (CW_HANDLE_IS_NULL(n)) {
        cw_array_free(&out);
        return cwed_fail(c, "no node '%s' to select (try: scene.tree)", path);
      }
      int at = -1;
      for (int i = 0; i < out.count; i++)
        if (out.data[i].id == n.id) at = i;
      bool remove = !strcmp(mode, "remove") || (!strcmp(mode, "toggle") && at >= 0);
      if (remove && at >= 0) memmove(out.data + at, out.data + at + 1, sizeof(cw_node) * (size_t)(out.count - at - 1)), out.count--;
      else if (!remove && at < 0) cw_array_push(&out, n);
    }
    p += l + (e ? 1 : 0);
  }
  cwed_selection_set(out.data, out.count);
  cw_array_free(&out);
  cwed_json *j = cwed_reply(c);
  cwed_json_key(j, "selection");
  cwed_json_raw(j, "[");
  for (int i = 0; i < d->sel.count; i++) {
    if (i) cwed_json_raw(j, ",");
    cwed_json_str(j, cwed_doc_node_path(d, d->sel.data[i]));
  }
  cwed_json_raw(j, "]");
  return true;
}

static bool cmd_select_clear(cwed_call *c) {
  (void)c;
  cwed_selection_set(NULL, 0);
  return true;
}

static bool cmd_select_all(cwed_call *c) {
  cwed_doc *d = cwed_call_doc(c);
  CW_ARRAY(cw_node) out = { 0 };
  cw_node r = d->root;
  for (int i = 0; i < cw_node_child_count(r); i++) cw_array_push(&out, cw_node_child(r, i));
  cwed_selection_set(out.data, out.count);
  cw_array_free(&out);
  cwed_json_key(cwed_reply(c), "count");
  cwed_json_printf(cwed_reply(c), "%d", d->sel.count);
  return true;
}

void cwed__selection_commands(void);
void cwed__selection_commands(void) {
  static const cwed_param sel_p[] = {
    { "paths", CWED_ARG_STRING, NULL, "node paths separated by commas (\".\" = the root)" },
    { "mode", CWED_ARG_STRING, "set", "set, add, remove or toggle" },
    { 0 } };
  cwed_command_register(&(cwed_command_desc){
    .name = "select", .title = "Select Nodes", .params = sel_p, .flags = CWED_CMD_SCENE | CWED_CMD_HIDDEN,
    .doc = "Change the selection every panel shares. Replies with the selected paths.", .exec = cmd_select });
  cwed_command_register(&(cwed_command_desc){
    .name = "select.clear", .title = "Select None", .flags = CWED_CMD_SCENE, .shortcut = "ctrl+shift+a",
    .doc = "Clear the selection.", .exec = cmd_select_clear });
  cwed_command_register(&(cwed_command_desc){
    .name = "select.all", .title = "Select All Top-Level Nodes", .flags = CWED_CMD_SCENE, .shortcut = "ctrl+a",
    .doc = "Select every child of the scene root.", .exec = cmd_select_all });
}

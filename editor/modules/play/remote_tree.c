/* Remote panel: the RUNNING game's scene tree, keyed by node uid.
 *
 * One `tree` snapshot per launch, then `changes <revision>` every
 * play.poll_frames frames: removed / renamed / moved apply in place, an added
 * node's subtree comes from `tree #uid`, and a `resync` (tracking just began,
 * too far behind, another run) takes a new snapshot. Selecting a row shows
 * that node in the Live inspector. Rows the game added that the edited scene
 * does not have are marked "(runtime)". */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "play.h"

typedef struct {
  int uid, parent;
  bool alive;
  char name[64], type[48];
  int *kids, nkids, capkids;
} rnode;

static struct {
  rnode *n;
  int count, cap;
  int *slot; /* uid -> index + 1 */
  int slot_cap;
  int root;  /* uid of /root */
  double rev;
  bool have, dirty, want_snapshot, inflight;
  int gen, frames, applied_events, snapshots, set_events;
} M = { .rev = -1 };

static cw_node tree, header;
static int *item_uid, n_items, cap_items; /* tree item index -> uid */
static bool syncing;
static int shown_inspected = -1;

/* ── model ────────────────────────────────────────────────────────────── */

static rnode *find(int uid) {
  if (uid <= 0 || uid >= M.slot_cap || !M.slot[uid]) return NULL;
  rnode *r = &M.n[M.slot[uid] - 1];
  return r->alive ? r : NULL;
}

static rnode *add_node(int uid, int parent, const char *name, const char *type) {
  if (uid <= 0) return NULL;
  rnode *e = find(uid);
  if (!e) {
    if (uid >= M.slot_cap) {
      int nc = M.slot_cap ? M.slot_cap : 256;
      while (nc <= uid) nc *= 2;
      M.slot = realloc(M.slot, sizeof *M.slot * (size_t)nc);
      memset(M.slot + M.slot_cap, 0, sizeof *M.slot * (size_t)(nc - M.slot_cap));
      M.slot_cap = nc;
    }
    if (M.count == M.cap) {
      M.cap = M.cap ? M.cap * 2 : 256;
      M.n = realloc(M.n, sizeof *M.n * (size_t)M.cap);
    }
    e = &M.n[M.count++];
    memset(e, 0, sizeof *e);
    M.slot[uid] = M.count;
  }
  e->uid = uid;
  e->parent = parent;
  e->alive = true;
  snprintf(e->name, sizeof e->name, "%s", name ? name : "?");
  snprintf(e->type, sizeof e->type, "%s", type ? type : "?");
  return e;
}

static void kid_insert(rnode *p, int uid, int index) {
  for (int i = 0; i < p->nkids; i++)
    if (p->kids[i] == uid) return;
  if (p->nkids == p->capkids) {
    p->capkids = p->capkids ? p->capkids * 2 : 4;
    p->kids = realloc(p->kids, sizeof *p->kids * (size_t)p->capkids);
  }
  if (index < 0 || index > p->nkids) index = p->nkids;
  memmove(p->kids + index + 1, p->kids + index, sizeof *p->kids * (size_t)(p->nkids - index));
  p->kids[index] = uid;
  p->nkids++;
}

static void kid_remove(rnode *p, int uid) {
  for (int i = 0; p && i < p->nkids; i++)
    if (p->kids[i] == uid) {
      memmove(p->kids + i, p->kids + i + 1, sizeof *p->kids * (size_t)(p->nkids - i - 1));
      p->nkids--;
      return;
    }
}

static void kill(int uid) {
  rnode *e = find(uid);
  if (!e) return;
  e->alive = false;
  for (int i = 0; i < e->nkids; i++) kill(e->kids[i]);
  e->nkids = 0;
}

static void clear_model(void) {
  for (int i = 0; i < M.count; i++) free(M.n[i].kids);
  M.count = 0;
  if (M.slot) memset(M.slot, 0, sizeof *M.slot * (size_t)M.slot_cap);
  M.have = false;
  M.root = 0;
  M.dirty = true;
}

/* a `tree` reply object into the model under parent */
static void load(const pj *o, int parent) {
  int uid = (int)pj_num(o, "uid", 0);
  rnode *e = add_node(uid, parent, pj_str(o, "name"), pj_str(o, "type"));
  if (!e) return;
  e->nkids = 0;
  pj *kids = pj_get(o, "children");
  if (!kids || kids->t != PJ_ARR) return;
  for (int i = 0; i < kids->count; i++) {
    int k = (int)pj_num(kids->items[i], "uid", 0);
    load(kids->items[i], uid);
    rnode *p = find(uid);
    if (p && find(k)) kid_insert(p, k, p->nkids);
  }
}

/* ── sync ─────────────────────────────────────────────────────────────── */

static void on_snapshot(const cw_remote_reply *r, void *u) {
  (void)u;
  M.inflight = false;
  if (!r->ok) return;
  pj *t = pj_parse(r->json);
  if (!t) return;
  clear_model();
  M.root = (int)pj_num(t, "uid", 0);
  load(t, 0);
  M.rev = pj_num(t, "revision", 0);
  M.have = true;
  M.want_snapshot = false;
  M.snapshots++;
  M.dirty = true;
  pj_free(t);
}

static void on_subtree(const cw_remote_reply *r, void *u) {
  int parent = (int)(intptr_t)u;
  if (!r->ok) return;
  pj *t = pj_parse(r->json);
  rnode *p = find(parent);
  int uid = (int)pj_num(t, "uid", 0);
  if (t && p && find(uid)) {
    load(t, parent);
    M.dirty = true;
  }
  pj_free(t);
}

static void on_changes(const cw_remote_reply *r, void *u) {
  (void)u;
  M.inflight = false;
  if (!r->ok) return;
  pj *c = pj_parse(r->json);
  if (!c) return;
  if (pj_bool(c, "resync", false)) {
    M.want_snapshot = true;
    pj_free(c);
    return;
  }
  pj *ev = pj_get(c, "events");
  int added = 0;
  int added_uids[16], added_parents[16];
  for (int i = 0; ev && i < ev->count; i++) {
    pj *e = ev->items[i];
    const char *kind = pj_str(e, "kind");
    int uid = (int)pj_num(e, "uid", 0);
    if (!kind) continue;
    M.applied_events++;
    if (!strcmp(kind, "added")) {
      int parent = (int)pj_num(e, "parent", 0);
      rnode *p = find(parent);
      if (!p) continue;
      add_node(uid, parent, pj_str(e, "name"), pj_str(e, "type"));
      kid_insert(find(parent), uid, (int)pj_num(e, "index", -1));
      if (added < 16) added_uids[added] = uid, added_parents[added] = parent;
      added++;
      M.dirty = true;
    } else if (!strcmp(kind, "removed")) {
      rnode *n = find(uid);
      if (n) kid_remove(find(n->parent), uid);
      kill(uid);
      M.dirty = true;
    } else if (!strcmp(kind, "renamed")) {
      rnode *n = find(uid);
      if (n && pj_str(e, "name")) snprintf(n->name, sizeof n->name, "%s", pj_str(e, "name"));
      M.dirty = true;
    } else if (!strcmp(kind, "moved")) {
      rnode *n = find(uid), *np = find((int)pj_num(e, "parent", 0));
      if (n && np) {
        kid_remove(find(n->parent), uid);
        n->parent = np->uid;
        kid_insert(np, uid, (int)pj_num(e, "index", -1));
      }
      M.dirty = true;
    } else if (!strcmp(kind, "set")) {
      M.set_events++;
    }
  }
  M.rev = pj_num(c, "revision", M.rev);
  bool more = pj_bool(c, "more", false);
  pj_free(c);
  /* an added node may bring a subtree (an instanced scene): fetch it, or
   * take a new snapshot when many arrived at once */
  if (added > 16) M.want_snapshot = true;
  else
    for (int i = 0; i < added; i++)
      pl_send(cw_frame_printf("tree #%d", added_uids[i]), on_subtree, (void *)(intptr_t)added_parents[i]);
  if (more) M.frames = -1; /* ask again next frame */
}

static void sync_tick(void) {
  if (!pl_running() || M.inflight) return;
  M.frames++;
  if (!M.have || M.want_snapshot) {
    M.inflight = true;
    if (!pl_send("tree /root", on_snapshot, NULL)) M.inflight = false;
    return;
  }
  if (M.frames % pl_poll_frames() != 0 && M.frames != 0) return;
  M.inflight = true;
  if (!pl_send(cw_frame_printf("changes %.0f 500", M.rev), on_changes, NULL)) M.inflight = false;
}

/* ── the widget ───────────────────────────────────────────────────────── */

static void add_items(int uid, int parent_item, cwed_doc *d) {
  rnode *e = find(uid);
  if (!e) return;
  int it = cw_uitree_create_item(tree, parent_item, e->name);
  /* a node the edited scene has, or one the game made at runtime */
  const char *note = "";
  if (d) {
    char path[1024] = "";
    int chain[64], depth = 0;
    for (rnode *p = e; p && p->uid != M.root && depth < 64; p = find(p->parent)) chain[depth++] = p->uid;
    for (int i = depth - 1; i >= 0; i--) {
      size_t l = strlen(path);
      snprintf(path + l, sizeof path - l, "/%s", find(chain[i])->name);
    }
    const char *dp = pl_doc_path(cw_frame_printf("/root%s", path));
    if (!dp || CW_HANDLE_IS_NULL(cwed_doc_node(d, dp))) note = depth > 0 ? "  (runtime)" : "";
  }
  cw_uitree_set_text(tree, it, 1, cw_frame_printf("%s%s", e->type, note));
  cw_uitree_set_metadata(tree, it, uid);
  if (it >= cap_items) {
    cap_items = it + 64;
    item_uid = realloc(item_uid, sizeof *item_uid * (size_t)cap_items);
  }
  for (int i = n_items; i <= it; i++) item_uid[i] = 0;
  if (it >= n_items) n_items = it + 1;
  item_uid[it] = uid;
  for (int i = 0; i < e->nkids; i++) add_items(e->kids[i], it, d);
}

static int item_of(int uid) {
  for (int i = 0; i < n_items; i++)
    if (item_uid[i] == uid) return i;
  return -1;
}

static void rebuild(void) {
  syncing = true;
  cw_uitree_clear(tree);
  n_items = 0;
  if (M.have) {
    rnode *root = find(M.root);
    cwed_doc *d = pl_doc();
    for (int i = 0; root && i < root->nkids; i++) add_items(root->kids[i], -1, d);
  }
  int it = item_of(pl_inspected());
  if (it >= 0) cw_uitree_select(tree, it);
  shown_inspected = pl_inspected();
  syncing = false;
  M.dirty = false;
}

static void on_select(cw_node t, const cw_value *a, int argc, void *user) {
  (void)argc, (void)user;
  if (syncing) return;
  int uid = (int)cw_uitree_metadata(t, (int)a[0].i);
  if (uid > 0) cwed_runf("play.inspect node=#%d", uid);
}

static cw_node create(void *user) {
  (void)user;
  cw_node box = cw_node_new("VBoxContainer", NULL);
  header = cw_node_new("Label", "Header");
  cw_node_set(header, "ellipsis", cw_val_bool(true));
  cw_node_add_child(box, header);
  tree = cw_node_new("Tree", "Nodes");
  cw_uitree_set_columns(tree, 2);
  cw_uitree_set_column_title(tree, 0, "Node");
  cw_uitree_set_column_title(tree, 1, "Type");
  cw_uitree_set_column_expand(tree, 0, true, 80);
  cw_uitree_set_column_expand(tree, 1, true, 80);
  cw_control_set_size_flags(tree, CW_SIZE_EXPAND_FILL, CW_SIZE_EXPAND_FILL);
  cw_connect(tree, "item_selected", (cw_node){ 0 }, NULL, on_select, NULL, 0);
  cw_node_add_child(box, tree);
  return box;
}

void pl_tree_frame(void) {
  if (!cw_node_valid(tree)) return;
  if (M.gen != pl_generation()) { /* a new launch: a new tree */
    M.gen = pl_generation();
    clear_model();
    M.inflight = false;
    M.want_snapshot = false;
    M.frames = -1;
    M.rev = -1;
  }
  sync_tick();
  if (M.dirty || shown_inspected != pl_inspected()) rebuild();
  int alive = 0;
  for (int i = 0; i < M.count; i++) alive += M.n[i].alive;
  if (!pl_available()) cw_label_set_text(header, "Play is not available in the cart editor (Game panel says why)");
  else if (!pl_active()) cw_label_set_text(header, "No game running: Play (F5) shows its live tree here");
  else if (!pl_running()) cw_label_set_text(header, cw_frame_printf("Game %s: the last tree it had", pl_status()));
  else cw_label_set_text(header, cw_frame_printf("Live: %s  ·  %d nodes  ·  rev %.0f", pl_scene(), alive, M.rev));
}

void pl_tree_json(cwed_json *j) {
  int alive = 0;
  for (int i = 0; i < M.count; i++) alive += M.n[i].alive;
  cwed_json_printf(j, "\"synced\":%s,\"revision\":%.0f,\"nodes\":%d,\"snapshots\":%d,\"events\":%d,\"set_events\":%d", M.have ? "true" : "false",
                   M.rev, alive, M.snapshots, M.applied_events, M.set_events);
  cwed_json_key(j, "rows");
  cwed_json_raw(j, "[");
  bool first = true;
  for (int i = 0; i < n_items; i++) {
    rnode *e = find(item_uid[i]);
    if (!e) continue;
    int depth = 0;
    for (int p = cw_uitree_item_parent(tree, i); p >= 0; p = cw_uitree_item_parent(tree, p)) depth++;
    cwed_json_raw(j, first ? "{" : ",{");
    first = false;
    cwed_json_printf(j, "\"uid\":%d,\"depth\":%d,\"name\":", e->uid, depth);
    cwed_json_str(j, e->name);
    cwed_json_raw(j, ",\"type\":");
    cwed_json_str(j, cw_uitree_text(tree, i, 1));
    cwed_json_raw(j, "}");
  }
  cwed_json_raw(j, "]");
}

void pl_tree_widgets(cwed_json *j) {
  cwed_json_key(j, "remote_rows");
  cwed_json_raw(j, "{");
  for (int i = 0; i < n_items; i++) {
    cw_rect r = cw_uitree_item_rect(tree, i);
    if (r.w > 0) pl_rect_json(j, cw_frame_printf("%d", item_uid[i]), r);
  }
  cwed_json_raw(j, "}");
}

void pl_tree_init(void) {
  cwed_panel_register(&(cwed_panel_desc){ .id = "remote_tree", .title = "Remote", .slot = "left_top", .order = 80, .create = create });
}

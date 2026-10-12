/* The scene side of a session: authority, RPC, NetSynchronizer, NetSpawner.
 *
 * RPC     [path][method][u8 argc][values]
 * SYNC    [path of the synchronizer][u32 seq][u8 count][(name, value)...]
 * SPAWN   [path of the spawner][u8 entry][name][i8 authority][u8 count][(name, value)...]
 * DESPAWN [path of the spawner][name]
 * Paths are relative to the session root. */
#include <stdio.h>
#include <stdlib.h>

#include "net_internal.h"
#include "cartwheel/core.h"
#include "cartwheel/scene_file.h"

/* ── authority ─────────────────────────────────────────────────────── */

void cw_net_set_authority(cw_node n, int player) {
  cw_net_session *s = cw_net_session_of(cw_node_tree(n));
  if (!s) {
    cw_warn("cw_net_set_authority: '%s' is not in a tree with a session (cw_net_attach)", cw_node_name(n));
    return;
  }
  cw__net_authority_set(s, cw_node_uid(n), player);
}
int cw_net_authority(cw_node n) {
  cw_net_session *s = cw_net_session_of(cw_node_tree(n));
  if (!s) return 0;
  for (cw_node c = n; cw_node_valid(c); c = cw_node_parent(c)) {
    bool found;
    int a = cw__net_authority_get(s, cw_node_uid(c), &found);
    if (found) return a;
  }
  return 0;
}
bool cw_net_is_authority(cw_node n) {
  cw_net_session *s = cw_net_session_of(cw_node_tree(n));
  return s && cw_net_authority(n) == cw_net_local_player(s);
}

static bool rel_path(cw_net_session *s, cw_node n, char *buf, int cap) {
  cw_node root = cw__net_root(s);
  if (!cw_node_valid(root) || !cw_node_valid(n)) return false;
  for (cw_node c = n; cw_node_valid(c); c = cw_node_parent(c))
    if (c.id == root.id) {
      cw_node_path(n, root, buf, cap);
      return true;
    }
  return false;
}
static cw_node by_path(cw_net_session *s, const char *path) {
  cw_node root = cw__net_root(s);
  if (!cw_node_valid(root) || !path) return (cw_node){ 0 };
  return path[0] ? cw_node_get(root, path) : root;
}

/* ── RPC ───────────────────────────────────────────────────────────── */

typedef struct rpc_cfg {
  cw_rpc_mode mode;
  cw_net_channel ch;
  bool call_local;
} rpc_cfg;
static cw_map rpc_cfgs; /* hash(type, method) -> rpc_cfg packed */
static bool rpc_ready;
static int rpc_sender = -1;
static uint64_t rpc_key(const char *type, const char *method) { return cw_hash_str(type) * 1000003u ^ cw_hash_str(method); }

void cw_net_rpc_config(const char *type, const char *method, cw_rpc_mode mode, cw_net_channel ch, bool call_local) {
  if (!rpc_ready) {
    cw_map_init(&rpc_cfgs, CW_MEM_CORE);
    rpc_ready = true;
  }
  uint64_t v = (uint64_t)mode | (uint64_t)ch << 8 | (uint64_t)call_local << 16 | 1ull << 24;
  cw_map_set(&rpc_cfgs, rpc_key(type, method), v);
}
static bool rpc_find(cw_node n, const char *method, rpc_cfg *out) {
  if (!rpc_ready) return false;
  for (const cw_node_type *t = cw_node_get_type(n); t; t = t->base) {
    uint64_t v;
    if (cw_map_get(&rpc_cfgs, rpc_key(t->name, method), &v)) {
      *out = (rpc_cfg){ (cw_rpc_mode)(v & 0xff), (cw_net_channel)((v >> 8) & 0xff), ((v >> 16) & 1) != 0 };
      return true;
    }
  }
  return false;
}
int cw_net_rpc_sender(void) { return rpc_sender; }

static bool rpc_send(cw_node n, int player, const char *method, const cw_value *args, int argc) {
  cw_net_session *s = cw_net_session_of(cw_node_tree(n));
  rpc_cfg cfg;
  if (!s) {
    cw_warn("cw_net_rpc: '%s' is not in a tree with a session (cw_net_attach)", cw_node_name(n));
    return false;
  }
  if (!rpc_find(n, method, &cfg)) {
    cw_warn("cw_net_rpc: %s.%s is not an RPC (cw_net_rpc_config first)", cw_node_get_type(n)->name, method);
    return false;
  }
  if (cfg.mode == CW_RPC_AUTHORITY && !cw_net_is_authority(n)) {
    cw_warn("cw_net_rpc: %s.%s is authority-only and player %d is not the authority of '%s'", cw_node_get_type(n)->name,
            method, cw_net_local_player(s), cw_node_name(n));
    return false;
  }
  char path[512];
  if (!rel_path(s, n, path, sizeof path)) {
    cw_warn("cw_net_rpc: '%s' is outside the session root", cw_node_name(n));
    return false;
  }
  uint8_t buf[CW_NET_MAX_MESSAGE];
  cw__wbuf w = { buf, 0, sizeof buf, false };
  cw__w_str(&w, path);
  cw__w_str(&w, method);
  cw__w_u8(&w, (uint8_t)argc);
  for (int i = 0; i < argc; i++) cw__w_value(&w, &args[i], cw__net_root(s));
  if (w.overflow) {
    cw_error("cw_net_rpc: %s.%s arguments exceed one message (%d bytes)", cw_node_get_type(n)->name, method, CW_NET_MAX_MESSAGE);
    return false;
  }
  bool sent = cw_net_send(s, player, cfg.ch, CW__NM_RPC, buf, w.len);
  if (!sent && player != CW_NET_ALL) {
    cw_warn("cw_net_rpc_to: player %d is not connected (%s.%s not sent)", player, cw_node_get_type(n)->name, method);
    return false;
  }
  if (cfg.call_local && player == CW_NET_ALL) {
    int prev = rpc_sender;
    rpc_sender = cw_net_local_player(s);
    cw_node_call(n, method, args, argc, NULL);
    rpc_sender = prev;
  }
  return true;
}
bool cw_net_rpc(cw_node n, const char *method, const cw_value *args, int argc) { return rpc_send(n, CW_NET_ALL, method, args, argc); }
bool cw_net_rpc_to(cw_node n, int player, const char *method, const cw_value *args, int argc) {
  return rpc_send(n, player, method, args, argc);
}

static void on_rpc(cw_net_session *s, int from, const uint8_t *data, int len, void *user) {
  (void)user;
  cw__rbuf r = { data, len, 0, false };
  const char *path = cw__r_str(&r), *method = cw__r_str(&r);
  int argc = cw__r_u8(&r);
  cw_value args[16];
  if (r.bad || argc > 16) return;
  for (int i = 0; i < argc; i++)
    if (!cw__r_value(&r, &args[i], cw__net_root(s))) return;
  cw_node n = by_path(s, path);
  rpc_cfg cfg;
  if (!cw_node_valid(n)) return; /* not spawned here (yet) */
  if (!rpc_find(n, method, &cfg)) {
    cw_warn("net: player %d called %s on '%s', which is not an RPC: refused", from, method, path);
    return;
  }
  if (cfg.mode == CW_RPC_AUTHORITY && cw_net_authority(n) != from) {
    cw_warn("net: player %d called authority-only %s on '%s' (authority %d): refused", from, method, path, cw_net_authority(n));
    return;
  }
  int prev = rpc_sender;
  rpc_sender = from;
  cw_node_call(n, method, args, argc, NULL);
  rpc_sender = prev;
}

/* ── NetSynchronizer / NetSpawner node types ───────────────────────── */

typedef struct netsync {
  const char *root_path, *properties;
  float interval;
  bool reliable;
  double last_sent;
  uint32_t seq_out, seq_in;
  bool any_in;
} netsync;
static const cw_prop netsync_props[] = {
  CW_PROP_FIELD(netsync, root_path, CW_T_STRING, CW_PROP_DEFAULT, "path from this node to the node whose properties replicate"),
  CW_PROP_FIELD(netsync, properties, CW_T_STRING, CW_PROP_DEFAULT, "comma-separated property names"),
  CW_PROP_FIELD(netsync, interval, CW_T_FLOAT, CW_PROP_DEFAULT, "seconds between updates (0 = every poll)"),
  CW_PROP_FIELD(netsync, reliable, CW_T_BOOL, CW_PROP_DEFAULT, "send on the reliable channel"),
  CW_PROP_END,
};
static void netsync_init(cw_node n, void *d) {
  (void)n;
  netsync *s = d;
  s->root_path = cw_intern("..");
  s->properties = cw_intern("position");
  s->last_sent = -1e9;
}
static void netsync_enter(cw_node n, void *d) { (void)d; cw_node_add_group(n, "cw.net.sync"); }

typedef struct netspawn {
  const char *spawn_path, *spawnable, *spawn_properties;
  cw_map known; /* child uid -> interned name */
  bool ready;
} netspawn;
static const cw_prop netspawn_props[] = {
  CW_PROP_FIELD(netspawn, spawn_path, CW_T_STRING, CW_PROP_DEFAULT, "path from this node to the node whose children replicate"),
  CW_PROP_FIELD(netspawn, spawnable, CW_T_STRING, CW_PROP_DEFAULT, "comma-separated scene paths (*.cwscene) or node type names"),
  CW_PROP_FIELD(netspawn, spawn_properties, CW_T_STRING, CW_PROP_DEFAULT, "properties sent with each spawn"),
  CW_PROP_END,
};
static void netspawn_init(cw_node n, void *d) {
  (void)n;
  netspawn *s = d;
  s->spawn_path = cw_intern("..");
  s->spawnable = cw_intern("");
  s->spawn_properties = cw_intern("");
  cw_map_init(&s->known, CW_MEM_CORE);
  s->ready = true;
}
static void netspawn_destroy(cw_node n, void *d) {
  (void)n;
  netspawn *s = d;
  if (s->ready) {
    for (int k = 0; k < s->known.count; k++)
      if (cw_map_live(&s->known, k)) cw_intern_unref((const char *)(uintptr_t)s->known.vals[k]);
    cw_map_free(&s->known);
  }
  s->ready = false;
}
static void netspawn_enter(cw_node n, void *d) { (void)d; cw_node_add_group(n, "cw.net.spawn"); }

static cw_node_type netsync_type = {
  .name = "NetSynchronizer", .parent = "Node", .data_size = sizeof(netsync), .props = netsync_props,
  .doc = "Replicates properties of root_path from its network authority to every other peer (cartwheel/net.h).",
  .init = netsync_init, .enter_tree = netsync_enter
};
static cw_node_type netspawn_type = {
  .name = "NetSpawner", .parent = "Node", .data_size = sizeof(netspawn), .props = netspawn_props,
  .doc = "Replicates creation and removal of spawn_path's children (listed scenes or types) from its authority (cartwheel/net.h).",
  .init = netspawn_init, .destroy = netspawn_destroy, .enter_tree = netspawn_enter
};

void cw_net_register_types(void) {
  if (cw_node_type_find("NetSynchronizer")) return;
  cw_scene_register_builtin_types();
  cw_node_type_register(&netsync_type);
  cw_node_type_register(&netspawn_type);
}

/* iterate a comma list: copies item i into buf; false past the end */
static bool list_item(const char *list, int i, char *buf, int cap) {
  if (!list) return false;
  const char *p = list;
  for (int k = 0;; k++) {
    while (*p == ' ') p++;
    const char *e = strchr(p, ',');
    int n = e ? (int)(e - p) : (int)strlen(p);
    while (n > 0 && p[n - 1] == ' ') n--;
    if (k == i) {
      if (n == 0 && !e) return false;
      n = CW_MIN(n, cap - 1);
      memcpy(buf, p, (size_t)n);
      buf[n] = 0;
      return true;
    }
    if (!e) return false;
    p = e + 1;
  }
}

static void write_props(cw__wbuf *w, cw_net_session *s, cw_node target, const char *list) {
  int count_at = w->len, count = 0;
  cw__w_u8(w, 0);
  char name[128];
  for (int i = 0; list_item(list, i, name, sizeof name); i++) {
    cw_value v;
    if (!cw_node_get_prop(target, name, &v)) continue;
    cw__w_str(w, name);
    cw__w_value(w, &v, cw__net_root(s));
    count++;
  }
  if (!w->overflow) w->p[count_at] = (uint8_t)count;
}
static void read_props(cw__rbuf *r, cw_net_session *s, cw_node target) {
  int count = cw__r_u8(r);
  for (int i = 0; i < count && !r->bad; i++) {
    const char *name = cw__r_str(r);
    cw_value v;
    if (!cw__r_value(r, &v, cw__net_root(s)) || !name) return;
    if (cw_node_valid(target)) cw_node_set(target, name, v);
  }
}

static void sync_poll(cw_net_session *s, cw_tree *t) {
  cw_node *nodes;
  int n = cw_tree_group(t, "cw.net.sync", &nodes);
  for (int i = 0; i < n; i++) {
    netsync *d = cw_node_data_t(nodes[i], &netsync_type);
    cw_node root = cw_node_get(nodes[i], d->root_path);
    if (!cw_node_valid(root) || !cw_net_is_authority(root) || !cw_net_peer_count(s)) continue;
    if (cw_net_now(s) - d->last_sent < d->interval) continue;
    char path[512];
    if (!rel_path(s, nodes[i], path, sizeof path)) continue;
    uint8_t buf[CW_NET_MAX_MESSAGE];
    cw__wbuf w = { buf, 0, sizeof buf, false };
    cw__w_str(&w, path);
    cw__w_u32(&w, ++d->seq_out);
    write_props(&w, s, root, d->properties);
    if (w.overflow) {
      cw_error("NetSynchronizer '%s': its properties exceed one message", path);
      continue;
    }
    cw_net_send(s, CW_NET_ALL, d->reliable ? CW_NET_RELIABLE : CW_NET_UNRELIABLE, CW__NM_SYNC, buf, w.len);
    d->last_sent = cw_net_now(s);
  }
}
static void on_sync(cw_net_session *s, int from, const uint8_t *data, int len, void *user) {
  (void)user;
  cw__rbuf r = { data, len, 0, false };
  cw_node n = by_path(s, cw__r_str(&r));
  uint32_t seq = cw__r_u32(&r);
  if (r.bad || !cw_node_valid(n) || !cw_node_is(n, "NetSynchronizer")) return;
  netsync *d = cw_node_data_t(n, &netsync_type);
  cw_node root = cw_node_get(n, d->root_path);
  if (!cw_node_valid(root) || cw_net_authority(root) != from) return; /* only the authority writes */
  if (d->any_in && (int32_t)(seq - d->seq_in) <= 0) return;       /* older than what we applied */
  d->any_in = true;
  d->seq_in = seq;
  read_props(&r, s, root);
}

/* which spawnable entry made this child; -1 = none */
static int spawn_entry(const netspawn *d, cw_node child) {
  const char *src = cw_scene_instance_source(child);
  const char *type = cw_node_get_type(child)->name;
  char item[256];
  for (int i = 0; list_item(d->spawnable, i, item, sizeof item); i++) {
    size_t l = strlen(item);
    bool scene = l > 8 && !strcmp(item + l - 8, ".cwscene");
    if (scene ? (src && !strcmp(src, item)) : !strcmp(type, item)) return i;
  }
  return -1;
}
static void send_spawn(cw_net_session *s, int player, cw_node spawner, const netspawn *d, cw_node child, int entry) {
  char path[512];
  if (!rel_path(s, spawner, path, sizeof path)) return;
  uint8_t buf[CW_NET_MAX_MESSAGE];
  cw__wbuf w = { buf, 0, sizeof buf, false };
  cw__w_str(&w, path);
  cw__w_u8(&w, (uint8_t)entry);
  cw__w_str(&w, cw_node_name(child));
  bool found;
  int a = cw__net_authority_get(s, cw_node_uid(child), &found);
  cw__w_u8(&w, (uint8_t)(int8_t)(found ? a : -1));
  write_props(&w, s, child, d->spawn_properties);
  if (!w.overflow) cw_net_send(s, player, CW_NET_RELIABLE, CW__NM_SPAWN, buf, w.len);
}
static void spawn_poll(cw_net_session *s, cw_tree *t) {
  cw_node *nodes;
  int n = cw_tree_group(t, "cw.net.spawn", &nodes);
  for (int i = 0; i < n; i++) {
    netspawn *d = cw_node_data_t(nodes[i], &netspawn_type);
    cw_node parent = cw_node_get(nodes[i], d->spawn_path);
    if (!cw_node_valid(parent) || !cw_net_is_authority(nodes[i])) continue;
    /* children that appeared */
    int cc = cw_node_child_count(parent);
    for (int k = 0; k < cc; k++) {
      cw_node c = cw_node_child(parent, k);
      uint64_t v;
      if (cw_map_get(&d->known, cw_node_uid(c), &v)) continue;
      int entry = spawn_entry(d, c);
      if (entry < 0) continue;
      /* a reference: the despawn names the child after it is freed */
      cw_map_set(&d->known, cw_node_uid(c), (uint64_t)(uintptr_t)cw_intern_ref(cw_node_name(c)));
      send_spawn(s, CW_NET_ALL, nodes[i], d, c, entry);
    }
    /* children that went away (collected first: removing may compact the map) */
    char path[512];
    CW_ARRAY(uint64_t) gone = { 0 };
    for (int k = 0; k < d->known.count; k++) {
      if (!cw_map_live(&d->known, k)) continue;
      cw_node c = cw_node_by_uid(t, d->known.keys[k]);
      if (cw_node_valid(c) && cw_node_parent(c).id == parent.id) continue;
      const char *name = (const char *)(uintptr_t)d->known.vals[k];
      if (rel_path(s, nodes[i], path, sizeof path)) {
        uint8_t buf[600];
        cw__wbuf w = { buf, 0, sizeof buf, false };
        cw__w_str(&w, path);
        cw__w_str(&w, name);
        if (!w.overflow) cw_net_send(s, CW_NET_ALL, CW_NET_RELIABLE, CW__NM_DESPAWN, buf, w.len);
      }
      cw_array_push(&gone, d->known.keys[k]);
    }
    for (int k = 0; k < gone.count; k++) {
      uint64_t v = 0;
      cw_map_get(&d->known, gone.data[k], &v);
      cw_intern_unref((const char *)(uintptr_t)v);
      cw_map_remove(&d->known, gone.data[k]);
    }
    cw_array_free(&gone);
  }
}
static void on_spawn(cw_net_session *s, int from, const uint8_t *data, int len, void *user) {
  (void)user;
  cw__rbuf r = { data, len, 0, false };
  cw_node sp = by_path(s, cw__r_str(&r));
  int entry = cw__r_u8(&r);
  const char *name = cw__r_str(&r);
  int auth = (int8_t)cw__r_u8(&r);
  if (r.bad || !name || !cw_node_valid(sp) || !cw_node_is(sp, "NetSpawner") || cw_net_authority(sp) != from) return;
  netspawn *d = cw_node_data_t(sp, &netspawn_type);
  cw_node parent = cw_node_get(sp, d->spawn_path);
  char item[256];
  if (!cw_node_valid(parent) || !list_item(d->spawnable, entry, item, sizeof item)) return;
  cw_node existing = cw_node_get(parent, name);
  if (cw_node_valid(existing)) return; /* already here (a duplicate spawn) */
  size_t l = strlen(item);
  cw_node c = l > 8 && !strcmp(item + l - 8, ".cwscene") ? cw_scene_instance_linked(item) : cw_node_new(item, name);
  if (!cw_node_valid(c)) {
    cw_warn("net: cannot spawn '%s' (%s)", name, item);
    return;
  }
  cw_node_set_name(c, name);
  read_props(&r, s, c);
  cw_node_add_child(parent, c);
  if (auth >= 0) cw__net_authority_set(s, cw_node_uid(c), auth);
}
static void on_despawn(cw_net_session *s, int from, const uint8_t *data, int len, void *user) {
  (void)user;
  cw__rbuf r = { data, len, 0, false };
  cw_node sp = by_path(s, cw__r_str(&r));
  const char *name = cw__r_str(&r);
  if (r.bad || !name || !cw_node_valid(sp) || !cw_node_is(sp, "NetSpawner") || cw_net_authority(sp) != from) return;
  netspawn *d = cw_node_data_t(sp, &netspawn_type);
  cw_node c = cw_node_get(cw_node_get(sp, d->spawn_path), name);
  if (cw_node_valid(c)) cw_node_free(c);
}

void cw__net_replicate_init(cw_net_session *s) {
  cw_net_on_message(s, CW__NM_RPC, on_rpc, NULL);
  cw_net_on_message(s, CW__NM_SYNC, on_sync, NULL);
  cw_net_on_message(s, CW__NM_SPAWN, on_spawn, NULL);
  cw_net_on_message(s, CW__NM_DESPAWN, on_despawn, NULL);
}
void cw__net_replicate_free(cw_net_session *s) { (void)s; }
void cw__net_replicate_poll(cw_net_session *s) {
  cw_tree *t = cw__net_tree(s);
  if (!t || !cw_node_valid(cw__net_root(s))) return;
  spawn_poll(s, t);
  sync_poll(s, t);
}
void cw__net_replicate_peer_joined(cw_net_session *s, int player) {
  cw_tree *t = cw__net_tree(s);
  if (!t || !cw_node_valid(cw__net_root(s))) return;
  cw_node *nodes;
  int n = cw_tree_group(t, "cw.net.spawn", &nodes);
  for (int i = 0; i < n; i++) {
    netspawn *d = cw_node_data_t(nodes[i], &netspawn_type);
    cw_node parent = cw_node_get(nodes[i], d->spawn_path);
    if (!cw_node_valid(parent) || !cw_net_is_authority(nodes[i])) continue;
    for (int k = 0; k < cw_node_child_count(parent); k++) {
      cw_node c = cw_node_child(parent, k);
      uint64_t v;
      if (!cw_map_get(&d->known, cw_node_uid(c), &v)) continue; /* the next poll announces it to everyone */
      send_spawn(s, player, nodes[i], d, c, spawn_entry(d, c));
    }
  }
}

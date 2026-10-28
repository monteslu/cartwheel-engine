/* The crowd (nav.h): agents that follow navmesh paths and avoid each other.
 *
 * A step runs four phases, each a range job over agents (or over the paths
 * planned this step), and each agent's work reads only what the step started
 * with and writes only its own slot:
 *   1. plan:   the oldest waiting agents (request order) get a corridor, a
 *              budgeted A* search each;
 *   2. grid:   positions and velocities are copied into flat arrays and the
 *              agents counting-sorted into grid cells (index order); then each
 *              agent finds its nearest neighbours (distance, then index; SIMD
 *              distance tests);
 *   3. steer:  corners along the corridor give the desired velocity; ORCA
 *              (third_party/rvo2) turns it into a velocity that avoids the
 *              neighbours' velocities from the step's start;
 *   4. move:   each agent slides along the navmesh surface and its corridor
 *              follows.
 * So a step gives the same result at any worker count, and every host gives
 * the same bits (no libm transcendentals, no FMA, no estimates). */
#include <float.h>
#include <math.h>
#include <string.h>

#include "cartwheel/jobs.h"
#include "nav_internal.h"
#include "simd4.h"
#include "third_party/rvo2/orca.h"

#define CORRIDOR_MAX 256
#define MAX_CORNERS 4
#define MAX_NB CW_CROWD_MAX_NEIGHBORS
#define MAX_VISITED 16
#define STRAIGHTPATH_END 0x02 /* Detour's DT_STRAIGHTPATH_END */
#define STRAIGHTPATH_LINK 0x04 /* Detour's DT_STRAIGHTPATH_OFFMESH_CONNECTION: the corner starts a link */
#define LINK_TRIGGER 2.25f    /* a link starts within radius x this of its first corner (as DetourCrowd) */

typedef struct agent {
  uint32_t gen;  /* odd: live */
  cw_crowd_agent_params p;
  float pos[3], vel[3], dvel[3], nvel[3];
  float target[3], goal[3]; /* goal: the target, or the corridor end's closest point to it */
  cw__poly target_ref;
  float vreq[3]; /* CW_CROWD_VELOCITY */
  cw_crowd_agent_status status;
  bool partial;
  uint32_t path_version;
  uint64_t plan_seq; /* request order while WAITING */
  int npath;         /* corridor length (corridor[0]: the polygon the agent is on) */
  float corners[MAX_CORNERS * 3];
  uint8_t corner_flags[MAX_CORNERS];
  cw__poly corner_refs[MAX_CORNERS];
  int ncorners;
  int nnb;
  int nb[MAX_NB];
  /* a link crossing */
  uint32_t link;              /* its id while crossing (0: not on a link) */
  float link_a[3], link_b[3]; /* from (where it stood) and to (the far end) */
  float link_t, link_dur, link_arc;
  uint8_t link_event;         /* this step: 1 began, 2 landed (reported after the step) */
  uint32_t event_link;        /* the link the event is about */
} agent;

typedef struct crowd {
  cw_navmesh nm;
  int max;
  agent *agents;
  cw__poly *corridors; /* max * CORRIDOR_MAX */
  CW_ARRAY(int) free_slots;
  int live;
  cw_nav_filter filter;
  int plan_per_step, plan_iters;
  uint64_t seq;
  uint32_t nav_version;
  /* step scratch */
  float *px, *py, *pz, *vx, *vz, *rad, *hgt, *pri;
  uint8_t *avoids, *alive;
  int *cell_of, *cell_start, *cell_items;
  int cells_cap;
  int gw, gh;
  float cell, gx0, gz0;
  int *plan_list;
  float dt;
  cw_crowd_stats stats;
  cw_crowd_link_fn link_fn;
  void *link_user;
} crowd;

static cw_pool g_crowds;
static bool g_init;

static crowd *get(cw_crowd h) { return g_init ? cw_pool_get(&g_crowds, h.id) : NULL; }

static agent *agent_of(crowd *c, cw_crowd_agent a) {
  /* the handle's index is the slot + 1, so that a handle cut to 32 bits (index only) is never the null handle */
  uint32_t i = CW_HANDLE_INDEX(a), g = CW_HANDLE_GEN(a);
  if (CW_HANDLE_IS_TRUNCATED(a)) cw_handle_report_truncated(a.id, "cw_crowd_*");
  if (!c || !a.id || !i || i > (uint32_t)c->max) return NULL;
  agent *ag = &c->agents[i - 1];
  return (ag->gen & 1) && ag->gen == g ? ag : NULL;
}

static int merge_start_moved(cw__poly *path, int npath, int max, const cw__poly *visited, int nvisited);

static cw__poly *corridor(crowd *c, const agent *a) { return &c->corridors[(size_t)(a - c->agents) * CORRIDOR_MAX]; }

cw_crowd_agent_params cw_crowd_agent_params_default(void) {
  return (cw_crowd_agent_params){ .radius = 0.5f, .height = 2, .max_speed = 3.5f, .max_accel = 8, .neighbor_dist = 6,
                                  .max_neighbors = 10, .time_horizon = 2, .arrive_dist = 0.3f, .slowdown_dist = 1.5f,
                                  .avoidance = true, .avoidance_priority = 0.5f, .manual_links = false };
}

static cw_crowd_agent_params clean_params(const cw_crowd_agent_params *p) {
  cw_crowd_agent_params q = p ? *p : cw_crowd_agent_params_default();
  if (q.max_neighbors > MAX_NB) q.max_neighbors = MAX_NB;
  if (q.max_neighbors < 0) q.max_neighbors = 0;
  if (!(q.radius > 0)) q.radius = 0.01f;
  if (!(q.time_horizon > 0)) q.time_horizon = 0.01f;
  if (q.avoidance_priority < 0) q.avoidance_priority = 0;
  return q;
}

/* ── lifecycle ────────────────────────────────────────────────────────── */

cw_crowd cw_crowd_new(cw_navmesh nm, int max_agents) {
  cw_crowd none = { 0 };
  if (!cw_navmesh_valid(nm) || max_agents <= 0) {
    cw_error("nav: cw_crowd_new needs a navmesh and max_agents > 0");
    return none;
  }
  if (!g_init) {
    cw_pool_init(&g_crowds, sizeof(crowd), CW_MEM_NAV);
    g_init = true;
  }
  uint64_t id;
  crowd *c = cw_pool_alloc(&g_crowds, &id);
  c->nm = nm;
  c->max = max_agents;
  c->agents = cw_calloc((size_t)max_agents, sizeof(agent), CW_MEM_NAV);
  c->corridors = cw_calloc((size_t)max_agents * CORRIDOR_MAX, sizeof(cw__poly), CW_MEM_NAV);
  size_t n = (size_t)max_agents;
  c->px = cw_alloc(sizeof(float) * n, CW_MEM_NAV);
  c->py = cw_alloc(sizeof(float) * n, CW_MEM_NAV);
  c->pz = cw_alloc(sizeof(float) * n, CW_MEM_NAV);
  c->vx = cw_alloc(sizeof(float) * n, CW_MEM_NAV);
  c->vz = cw_alloc(sizeof(float) * n, CW_MEM_NAV);
  c->rad = cw_alloc(sizeof(float) * n, CW_MEM_NAV);
  c->hgt = cw_alloc(sizeof(float) * n, CW_MEM_NAV);
  c->pri = cw_alloc(sizeof(float) * n, CW_MEM_NAV);
  c->avoids = cw_alloc(n, CW_MEM_NAV);
  c->alive = cw_alloc(n, CW_MEM_NAV);
  c->cell_of = cw_alloc(sizeof(int) * n, CW_MEM_NAV);
  c->cell_items = cw_alloc(sizeof(int) * n, CW_MEM_NAV);
  c->plan_list = cw_alloc(sizeof(int) * n, CW_MEM_NAV);
  /* free slots popped lowest first */
  for (int i = max_agents - 1; i >= 0; i--) cw_array_push(&c->free_slots, i);
  c->filter = cw_nav_filter_default();
  c->plan_per_step = 8;
  c->plan_iters = 0;
  c->nav_version = cw__nav_version(nm);
  cw_crowd h = { id };
  cw__nav_attach_crowd(nm, h);
  return h;
}

static void destroy(cw_crowd h, crowd *c) {
  cw_free(c->agents);
  cw_free(c->corridors);
  cw_free(c->px), cw_free(c->py), cw_free(c->pz), cw_free(c->vx), cw_free(c->vz);
  cw_free(c->rad), cw_free(c->hgt), cw_free(c->pri), cw_free(c->avoids), cw_free(c->alive);
  cw_free(c->cell_of), cw_free(c->cell_items), cw_free(c->cell_start), cw_free(c->plan_list);
  cw_array_free(&c->free_slots);
  cw_pool_release(&g_crowds, h.id);
}

void cw__crowd_free_for_navmesh(cw_crowd h) {
  crowd *c = get(h);
  if (!c) return;
  cw__nav_detach_crowd(c->nm, h);
  destroy(h, c);
}

void cw_crowd_free(cw_crowd h) { cw__crowd_free_for_navmesh(h); }

bool cw_crowd_valid(cw_crowd h) { return get(h) != NULL; }

void cw_crowd_set_plan_budget(cw_crowd h, int paths_per_step, int max_iters) {
  crowd *c = get(h);
  if (!c) return;
  c->plan_per_step = paths_per_step > 0 ? paths_per_step : 1;
  c->plan_iters = max_iters > 0 ? max_iters : 0;
}

void cw_crowd_set_filter(cw_crowd h, const cw_nav_filter *f) {
  crowd *c = get(h);
  if (c) c->filter = f ? *f : cw_nav_filter_default();
}

/* ── agents ───────────────────────────────────────────────────────────── */

cw_crowd_agent cw_crowd_add(cw_crowd h, t3_vec3 p, const cw_crowd_agent_params *params) {
  cw_crowd_agent none = { 0 };
  crowd *c = get(h);
  if (!c) return none;
  if (!c->free_slots.count) {
    cw_warn("nav: crowd full (%d agents)", c->max);
    return none;
  }
  const float pp[3] = { p.x, p.y, p.z };
  float snapped[3];
  cw__poly ref;
  if (!cw__nav_nearest_ref(c->nm, pp, &c->filter, &ref, snapped)) {
    cw_warn("nav: agent at (%g, %g, %g) is off the navmesh", (double)p.x, (double)p.y, (double)p.z);
    return none;
  }
  int i = cw_array_pop(&c->free_slots);
  agent *a = &c->agents[i];
  uint32_t gen = a->gen + 1;
  memset(a, 0, sizeof *a);
  a->gen = gen;
  a->p = clean_params(params);
  memcpy(a->pos, snapped, sizeof a->pos);
  a->status = CW_CROWD_IDLE;
  a->npath = 1;
  corridor(c, a)[0] = ref;
  c->live++;
  return CW_HANDLE_MAKE(cw_crowd_agent, (uint32_t)i + 1, gen);
}

void cw_crowd_remove(cw_crowd h, cw_crowd_agent ah) {
  crowd *c = get(h);
  agent *a = agent_of(c, ah);
  if (!a) return;
  a->gen++;
  c->live--;
  /* keep the free list sorted descending so the lowest slot is reused first */
  int i = (int)(a - c->agents);
  cw_array_push(&c->free_slots, i);
  for (int k = c->free_slots.count - 1; k > 0 && c->free_slots.data[k - 1] < c->free_slots.data[k]; k--) {
    int t = c->free_slots.data[k];
    c->free_slots.data[k] = c->free_slots.data[k - 1];
    c->free_slots.data[k - 1] = t;
  }
}

int cw_crowd_agent_slot(cw_crowd h, cw_crowd_agent a) {
  crowd *c = get(h);
  return agent_of(c, a) ? (int)CW_HANDLE_INDEX(a) - 1 : -1;
}
bool cw_crowd_agent_valid(cw_crowd h, cw_crowd_agent a) { return agent_of(get(h), a) != NULL; }

void cw_crowd_set_params(cw_crowd h, cw_crowd_agent ah, const cw_crowd_agent_params *p) {
  agent *a = agent_of(get(h), ah);
  if (a) a->p = clean_params(p);
}

void cw_crowd_set_target(cw_crowd h, cw_crowd_agent ah, t3_vec3 target) {
  crowd *c = get(h);
  agent *a = agent_of(c, ah);
  if (!a) return;
  const float t[3] = { target.x, target.y, target.z };
  float s[3];
  cw__poly ref;
  if (!cw__nav_nearest_ref(c->nm, t, &c->filter, &ref, s)) {
    a->status = CW_CROWD_NO_PATH;
    memcpy(a->target, t, sizeof a->target);
    a->target_ref = 0;
    return;
  }
  memcpy(a->target, s, sizeof a->target);
  memcpy(a->goal, s, sizeof a->goal);
  a->target_ref = ref;
  a->status = CW_CROWD_WAITING;
  a->plan_seq = ++c->seq;
}

void cw_crowd_clear_target(cw_crowd h, cw_crowd_agent ah) {
  agent *a = agent_of(get(h), ah);
  if (!a) return;
  a->status = CW_CROWD_IDLE;
  a->npath = a->npath ? 1 : 0;
}

void cw_crowd_set_velocity(cw_crowd h, cw_crowd_agent ah, t3_vec3 v) {
  agent *a = agent_of(get(h), ah);
  if (!a) return;
  a->vreq[0] = v.x, a->vreq[1] = 0, a->vreq[2] = v.z;
  a->status = CW_CROWD_VELOCITY;
  a->npath = a->npath ? 1 : 0;
}

void cw_crowd_teleport(cw_crowd h, cw_crowd_agent ah, t3_vec3 p) {
  crowd *c = get(h);
  agent *a = agent_of(c, ah);
  if (!a) return;
  const float pp[3] = { p.x, p.y, p.z };
  float s[3];
  cw__poly ref;
  if (!cw__nav_nearest_ref(c->nm, pp, &c->filter, &ref, s)) return;
  a->link = 0; /* a teleport ends a crossing */
  memcpy(a->pos, s, sizeof a->pos);
  memset(a->vel, 0, sizeof a->vel);
  corridor(c, a)[0] = ref;
  a->npath = 1;
  if (a->status == CW_CROWD_MOVING || a->status == CW_CROWD_ARRIVED || a->status == CW_CROWD_WAITING) {
    a->status = CW_CROWD_WAITING;
    a->plan_seq = ++c->seq;
  }
}

void cw_crowd_set_position(cw_crowd h, cw_crowd_agent ah, t3_vec3 p) {
  crowd *c = get(h);
  agent *a = agent_of(c, ah);
  if (!a || a->npath <= 0 || a->link) return; /* a crossing places the agent itself */
  cw__poly *path = corridor(c, a);
  const float to[3] = { p.x, p.y, p.z };
  float out[3];
  cw__poly visited[MAX_VISITED];
  int nv = cw__nav_move_along(c->nm, path[0], a->pos, to, &c->filter, out, visited, MAX_VISITED);
  if (nv <= 0) return;
  if (a->status == CW_CROWD_MOVING) a->npath = merge_start_moved(path, a->npath, CORRIDOR_MAX, visited, nv);
  else path[0] = visited[nv - 1], a->npath = 1;
  float hy;
  if (cw__nav_poly_height(c->nm, path[0], out, &hy)) out[1] = hy;
  memcpy(a->pos, out, sizeof a->pos);
}

bool cw_crowd_get_state(cw_crowd h, cw_crowd_agent ah, cw_crowd_agent_state *out) {
  crowd *c = get(h);
  agent *a = agent_of(c, ah);
  if (!a || !out) return false;
  out->position = (t3_vec3){ a->pos[0], a->pos[1], a->pos[2] };
  out->velocity = (t3_vec3){ a->vel[0], a->vel[1], a->vel[2] };
  out->desired_velocity = (t3_vec3){ a->dvel[0], a->dvel[1], a->dvel[2] };
  out->target = (t3_vec3){ a->target[0], a->target[1], a->target[2] };
  const float *nc = a->ncorners ? a->corners : a->pos;
  out->next_corner = (t3_vec3){ nc[0], nc[1], nc[2] };
  out->status = a->status;
  out->partial = a->partial;
  out->path_polys = a->npath;
  out->path_version = a->path_version;
  out->link = a->link;
  out->link_progress = a->link ? a->link_t : 0;
  out->link_start = a->link ? (t3_vec3){ a->link_a[0], a->link_a[1], a->link_a[2] } : (t3_vec3){ 0, 0, 0 };
  out->link_end = a->link ? (t3_vec3){ a->link_b[0], a->link_b[1], a->link_b[2] } : (t3_vec3){ 0, 0, 0 };
  if (a->link) out->status = CW_CROWD_ON_LINK;
  return true;
}

void cw_crowd_set_link_callback(cw_crowd h, cw_crowd_link_fn fn, void *user) {
  crowd *c = get(h);
  if (!c) return;
  c->link_fn = fn;
  c->link_user = user;
}

/* Lands a crossing: the agent stands on the link's far end (its corridor
 * already starts with the polygon there). */
static void land(crowd *c, agent *a) {
  cw__poly *path = corridor(c, a);
  float p[3];
  memcpy(p, a->link_b, sizeof p);
  if (a->npath > 0) {
    float hy;
    cw__nav_closest_on_poly(c->nm, path[0], a->link_b, p);
    if (cw__nav_poly_height(c->nm, path[0], p, &hy)) p[1] = hy;
  }
  memcpy(a->pos, p, sizeof a->pos);
  a->event_link = a->link;
  a->link = 0;
  a->link_event = 2;
}

bool cw_crowd_finish_link(cw_crowd h, cw_crowd_agent ah) {
  crowd *c = get(h);
  agent *a = agent_of(c, ah);
  if (!a || !a->link) return false;
  memset(a->vel, 0, sizeof a->vel);
  land(c, a);
  a->link_event = 0; /* the game ended it: no event */
  return true;
}

t3_vec3 cw_crowd_position(cw_crowd h, cw_crowd_agent ah) {
  agent *a = agent_of(get(h), ah);
  return a ? (t3_vec3){ a->pos[0], a->pos[1], a->pos[2] } : (t3_vec3){ 0, 0, 0 };
}

int cw_crowd_count(cw_crowd h) {
  crowd *c = get(h);
  return c ? c->live : 0;
}

int cw_crowd_agents(cw_crowd h, cw_crowd_agent *out, int cap) {
  crowd *c = get(h);
  if (!c) return 0;
  int n = 0;
  for (int i = 0; i < c->max; i++) {
    agent *a = &c->agents[i];
    if (!(a->gen & 1)) continue;
    if (out && n < cap) out[n] = CW_HANDLE_MAKE(cw_crowd_agent, (uint32_t)i + 1, a->gen);
    n++;
  }
  return n;
}

uint64_t cw_crowd_hash(cw_crowd h) {
  crowd *c = get(h);
  if (!c) return 0;
  uint64_t x = 0;
  for (int i = 0; i < c->max; i++) {
    agent *a = &c->agents[i];
    if (!(a->gen & 1)) continue;
    struct {
      int32_t i, status;
      float pos[3], vel[3];
    } rec = { i, a->link ? (int32_t)CW_CROWD_ON_LINK : (int32_t)a->status, { a->pos[0], a->pos[1], a->pos[2] },
              { a->vel[0], a->vel[1], a->vel[2] } };
    x = cw_hash_bytes(&rec, sizeof rec, x);
  }
  return x;
}

cw_crowd_stats cw_crowd_get_stats(cw_crowd h) {
  crowd *c = get(h);
  cw_crowd_stats s;
  memset(&s, 0, sizeof s);
  return c ? c->stats : s;
}

/* ── the corridor ─────────────────────────────────────────────────────── */

/* Detour's dtMergeCorridorStartMoved: the polygons moveAlongSurface crossed
 * replace the corridor's start up to the furthest one they share. */
static int merge_start_moved(cw__poly *path, int npath, int max, const cw__poly *visited, int nvisited) {
  int fp = -1, fv = -1;
  for (int i = npath - 1; i >= 0; i--) {
    bool found = false;
    for (int j = nvisited - 1; j >= 0; j--)
      if (path[i] == visited[j]) {
        fp = i;
        fv = j;
        found = true;
      }
    if (found) break;
  }
  if (fp == -1 || fv == -1) return npath;
  int req = nvisited - fv;
  int orig = fp + 1 < npath ? fp + 1 : npath;
  int size = npath - orig > 0 ? npath - orig : 0;
  if (req + size > max) size = max - req;
  if (size > 0) memmove(path + req, path + orig, (size_t)size * sizeof *path);
  for (int i = 0; i < req; i++) path[i] = visited[(nvisited - 1) - i];
  return req + size;
}

/* Detour's dtMergeCorridorStartShortcut: a raycast's polygons replace the
 * corridor's start up to the furthest one they share. */
static int merge_start_shortcut(cw__poly *path, int npath, int max, const cw__poly *visited, int nvisited) {
  int fp = -1, fv = -1;
  for (int i = npath - 1; i >= 0; i--) {
    bool found = false;
    for (int j = nvisited - 1; j >= 0; j--)
      if (path[i] == visited[j]) {
        fp = i;
        fv = j;
        found = true;
      }
    if (found) break;
  }
  if (fp == -1 || fv == -1) return npath;
  int req = fv;
  if (req <= 0) return npath;
  int size = npath - fp > 0 ? npath - fp : 0;
  if (req + size > max) size = max - req;
  if (size > 0) memmove(path + req, path + fp, (size_t)size * sizeof *path);
  for (int i = 0; i < req; i++) path[i] = visited[i];
  return req + size;
}

/* Detour's optimizePathVisibility: when the point ahead (the second corner)
 * is in straight sight, the corridor takes the sight line's polygons. An
 * agent pushed off its corridor would otherwise walk back to it. */
static bool optimize_visibility(crowd *c, agent *a, const float next[3]) {
  float dx = next[0] - a->pos[0], dz = next[2] - a->pos[2];
  float d = sqrtf(dx * dx + dz * dz);
  if (d < 0.01f) return false;
  float range = a->p.radius * 30;
  d = d + 0.01f < range ? d + 0.01f : range;
  float goal[3] = { a->pos[0] + dx * (range / d), a->pos[1], a->pos[2] + dz * (range / d) };
  cw__poly res[32];
  float t = 0;
  cw__poly *path = corridor(c, a);
  int nres = cw__nav_raycast_polys(c->nm, path[0], a->pos, goal, &c->filter, &t, res, 32);
  if (!(nres > 1 && t > 0.99f)) return false;
  a->npath = merge_start_shortcut(path, a->npath, CORRIDOR_MAX, res, nres);
  return true;
}

/* ── phase 1: plan ────────────────────────────────────────────────────── */

static void plan_range(int b, int e, int w, void *ctx) {
  crowd *c = ctx;
  for (int k = b; k < e; k++) {
    agent *a = &c->agents[c->plan_list[k]];
    cw__poly *path = corridor(c, a);
    cw__poly start = a->npath ? path[0] : 0;
    bool partial = false;
    int n = 0;
    if (start && a->target_ref)
      n = cw__nav_path_polys(c->nm, start, a->pos, a->target_ref, a->target, &c->filter, c->plan_iters, path,
                             CORRIDOR_MAX, &partial);
    if (n <= 0) {
      path[0] = start;
      a->npath = start ? 1 : 0;
      a->status = CW_CROWD_NO_PATH;
      a->partial = false;
      continue;
    }
    a->npath = n;
    a->partial = partial;
    if (partial) cw__nav_closest_on_poly(c->nm, path[n - 1], a->target, a->goal);
    else memcpy(a->goal, a->target, sizeof a->goal);
    a->status = CW_CROWD_MOVING;
    a->path_version++;
  }
  (void)w;
}

/* ── phase 2: grid and neighbours ─────────────────────────────────────── */

static void build_grid(crowd *c) {
  float bmin[3], bmax[3];
  cw__nav_bounds(c->nm, bmin, bmax);
  float range = 0;
  for (int i = 0; i < c->max; i++)
    if (c->alive[i] && c->agents[i].p.neighbor_dist > range) range = c->agents[i].p.neighbor_dist;
  if (range < 0.5f) range = 0.5f;
  float w = bmax[0] - bmin[0], d = bmax[2] - bmin[2];
  float cell = range;
  /* at most 256 x 256 cells: a bigger world gets bigger cells */
  if (w / cell > 256) cell = w / 256;
  if (d / cell > 256) cell = d / 256;
  c->cell = cell;
  c->gx0 = bmin[0];
  c->gz0 = bmin[2];
  c->gw = (int)(w / cell) + 1;
  c->gh = (int)(d / cell) + 1;
  int cells = c->gw * c->gh;
  if (cells + 1 > c->cells_cap) {
    cw_free(c->cell_start);
    c->cells_cap = cells + 1;
    c->cell_start = cw_alloc(sizeof(int) * (size_t)c->cells_cap, CW_MEM_NAV);
  }
  memset(c->cell_start, 0, sizeof(int) * (size_t)(cells + 1));
  for (int i = 0; i < c->max; i++) {
    if (!c->alive[i]) continue;
    int x = (int)((c->px[i] - c->gx0) / cell), z = (int)((c->pz[i] - c->gz0) / cell);
    x = x < 0 ? 0 : x >= c->gw ? c->gw - 1 : x;
    z = z < 0 ? 0 : z >= c->gh ? c->gh - 1 : z;
    c->cell_of[i] = z * c->gw + x;
    c->cell_start[c->cell_of[i] + 1]++;
  }
  for (int k = 0; k < cells; k++) c->cell_start[k + 1] += c->cell_start[k];
  int *fill = cw_alloc(sizeof(int) * (size_t)cells, CW_MEM_NAV);
  memcpy(fill, c->cell_start, sizeof(int) * (size_t)cells);
  for (int i = 0; i < c->max; i++)
    if (c->alive[i]) c->cell_items[fill[c->cell_of[i]]++] = i;
  cw_free(fill);
}

/* Keeps the k nearest by (d2, j): a total order, so the answer does not
 * depend on the order candidates were seen in. */
static void insert_nb(int *nb, float *nd, int *n, int k, int j, float d2) {
  int m = *n;
  if (m == k && !(d2 < nd[m - 1] || (d2 == nd[m - 1] && j < nb[m - 1]))) return;
  if (m < k) m++;
  int i = m - 1;
  while (i > 0 && (d2 < nd[i - 1] || (d2 == nd[i - 1] && j < nb[i - 1]))) {
    nb[i] = nb[i - 1];
    nd[i] = nd[i - 1];
    i--;
  }
  nb[i] = j;
  nd[i] = d2;
  *n = m;
}

static void neighbours_range(int b, int e, int w, void *ctx) {
  crowd *c = ctx;
  int cand[64 + 4];
  for (int i = b; i < e; i++) {
    if (!c->alive[i]) continue;
    agent *a = &c->agents[i];
    a->nnb = 0;
    int k = a->p.max_neighbors;
    if (k <= 0) continue;
    float r = a->p.neighbor_dist, r2 = r * r;
    float xi = c->px[i], zi = c->pz[i], yi = c->py[i], hi = c->hgt[i];
    int x0 = (int)((xi - r - c->gx0) / c->cell), x1 = (int)((xi + r - c->gx0) / c->cell);
    int z0 = (int)((zi - r - c->gz0) / c->cell), z1 = (int)((zi + r - c->gz0) / c->cell);
    x0 = x0 < 0 ? 0 : x0, z0 = z0 < 0 ? 0 : z0;
    x1 = x1 >= c->gw ? c->gw - 1 : x1, z1 = z1 >= c->gh ? c->gh - 1 : z1;
    float nd[MAX_NB];
    int n = 0;
    v4 vxi = v4_splat(xi), vzi = v4_splat(zi), vr2 = v4_splat(r2);
    for (int z = z0; z <= z1; z++)
      for (int x = x0; x <= x1; x++) {
        int cs = c->cell_start[z * c->gw + x], ce = c->cell_start[z * c->gw + x + 1];
        for (int s = cs; s < ce; s += 64) {
          int m = ce - s < 64 ? ce - s : 64;
          memcpy(cand, &c->cell_items[s], sizeof(int) * (size_t)m);
          for (int t = m; t < ((m + 3) & ~3); t++) cand[t] = i; /* padding: itself, skipped below */
          for (int t = 0; t < m; t += 4) {
            int j0 = cand[t], j1 = cand[t + 1], j2 = cand[t + 2], j3 = cand[t + 3];
            v4 dx = v4_sub(v4_set(c->px[j0], c->px[j1], c->px[j2], c->px[j3]), vxi);
            v4 dz = v4_sub(v4_set(c->pz[j0], c->pz[j1], c->pz[j2], c->pz[j3]), vzi);
            v4 d2 = v4_add(v4_mul(dx, dx), v4_mul(dz, dz));
            int bits = v4_bits(v4_lt(d2, vr2));
            if (!bits) continue;
            float fd[4];
            v4_store(fd, d2);
            for (int l = 0; l < 4; l++) {
              int j = cand[t + l];
              if (!((bits >> l) & 1) || j == i) continue;
              /* only agents whose heights overlap (another floor is not a neighbour) */
              float dy = c->py[j] - yi;
              float hh = (hi + c->hgt[j]) * 0.5f;
              if (dy > hh || -dy > hh) continue;
              insert_nb(a->nb, nd, &n, k, j, fd[l]);
            }
          }
        }
      }
    a->nnb = n;
  }
  (void)w;
}

/* ── phase 3: steer and avoid ─────────────────────────────────────────── */

static float len2(float x, float z) { return sqrtf(x * x + z * z); }

/* The corners from the agent on: the ones it already stands on are dropped
 * (never the end or a link's start), and none after a link's start (what
 * lies beyond it is reached by crossing). Returns the first kept index. */
static int trim_corners(const agent *a, const float *corners, const uint8_t *flags, int *n) {
  int first = 0;
  while (first < *n && !(flags[first] & (STRAIGHTPATH_END | STRAIGHTPATH_LINK)) &&
         len2(corners[first * 3] - a->pos[0], corners[first * 3 + 2] - a->pos[2]) < 0.01f)
    first++;
  for (int k = first; k < *n; k++)
    if (flags[k] & STRAIGHTPATH_LINK) {
      *n = k + 1;
      break;
    }
  return first;
}

static void steer(crowd *c, int i) {
  agent *a = &c->agents[i];
  a->dvel[0] = a->dvel[1] = a->dvel[2] = 0;
  a->ncorners = 0;
  if (a->link) { /* crossing a link: the move phase places it */
    a->nvel[0] = c->vx[i], a->nvel[1] = 0, a->nvel[2] = c->vz[i];
    return;
  }
  if (a->status == CW_CROWD_VELOCITY) {
    memcpy(a->dvel, a->vreq, sizeof a->dvel);
  } else if (a->status == CW_CROWD_MOVING && a->npath > 0) {
    float corners[MAX_CORNERS * 3 + 3];
    uint8_t flags[MAX_CORNERS + 1];
    cw__poly refs[MAX_CORNERS + 1];
    int n = cw__nav_corners(c->nm, a->pos, a->goal, corridor(c, a), a->npath, corners, flags, refs, MAX_CORNERS + 1);
    /* the straight path starts at the agent: drop corners it is already on */
    int first = trim_corners(a, corners, flags, &n);
    /* shortcut the corridor toward the corner after next, then find the corners again (not when the next
     * corner starts a link: a shortcut must not skip it) */
    if (n - first > 0 && !(flags[first] & STRAIGHTPATH_LINK)) {
      int ahead = first + (n - first > 1 ? 1 : 0);
      if (optimize_visibility(c, a, &corners[ahead * 3])) {
        n = cw__nav_corners(c->nm, a->pos, a->goal, corridor(c, a), a->npath, corners, flags, refs, MAX_CORNERS + 1);
        first = trim_corners(a, corners, flags, &n);
      }
    }
    a->ncorners = n - first < MAX_CORNERS ? n - first : MAX_CORNERS;
    memcpy(a->corners, &corners[first * 3], sizeof(float) * 3 * (size_t)a->ncorners);
    memcpy(a->corner_flags, &flags[first], (size_t)a->ncorners);
    memcpy(a->corner_refs, &refs[first], sizeof(cw__poly) * (size_t)a->ncorners);
    float gx = a->goal[0] - a->pos[0], gz = a->goal[2] - a->pos[2];
    float goal_dist = len2(gx, gz);
    bool last_is_end = a->ncorners > 0 && (a->corner_flags[a->ncorners - 1] & STRAIGHTPATH_END);
    if (a->ncorners == 0 || (last_is_end && a->ncorners == 1 && goal_dist <= a->p.arrive_dist)) {
      a->status = CW_CROWD_ARRIVED;
    } else {
      float dx = a->corners[0] - a->pos[0], dz = a->corners[2] - a->pos[2];
      float d = len2(dx, dz);
      float speed = a->p.max_speed;
      /* slow down only on the last leg, near the goal */
      if (last_is_end && a->ncorners == 1 && goal_dist < a->p.slowdown_dist && a->p.slowdown_dist > 0)
        speed = speed * (goal_dist / a->p.slowdown_dist);
      if (d > 0) {
        a->dvel[0] = dx / d * speed;
        a->dvel[2] = dz / d * speed;
      }
    }
  }
  float nv[2] = { a->dvel[0], a->dvel[2] };
  if (a->p.avoidance && a->nnb > 0) {
    cw_orca_input in;
    float vxi = c->vx[i], vzi = c->vz[i], pri = c->pri[i];
    for (int k = 0; k < a->nnb; k++) {
      int j = a->nb[k];
      in.rpx[k] = c->px[j] - c->px[i];
      in.rpy[k] = c->pz[j] - c->pz[i];
      in.rvx[k] = vxi - c->vx[j];
      in.rvy[k] = vzi - c->vz[j];
      in.cr[k] = c->rad[i] + c->rad[j];
      float sum = pri + c->pri[j];
      in.weight[k] = !c->avoids[j] ? 1.0f : sum > 0 ? c->pri[j] / sum : 0.5f;
      in.side[k] = i < j ? -1.0f : 1.0f;
    }
    for (int k = a->nnb; k < ((a->nnb + 3) & ~3); k++) {
      in.rpx[k] = in.rpy[k] = in.rvx[k] = in.rvy[k] = 0;
      in.cr[k] = 1, in.weight[k] = 0, in.side[k] = 1;
    }
    cw_orca_line lines[MAX_NB];
    cw_orca_lines(&in, a->nnb, vxi, vzi, 1.0f / a->p.time_horizon, 1.0f / c->dt, lines);
    float out[2];
    cw_orca_solve(lines, a->nnb, a->p.max_speed, nv, out);
    nv[0] = out[0], nv[1] = out[1];
  }
  /* acceleration limit */
  float ax = nv[0] - c->vx[i], az = nv[1] - c->vz[i];
  float al = len2(ax, az), amax = a->p.max_accel * c->dt;
  if (a->p.max_accel > 0 && al > amax) {
    ax = ax / al * amax;
    az = az / al * amax;
  }
  a->nvel[0] = c->vx[i] + ax;
  a->nvel[1] = 0;
  a->nvel[2] = c->vz[i] + az;
}

static void steer_range(int b, int e, int w, void *ctx) {
  crowd *c = ctx;
  for (int i = b; i < e; i++)
    if (c->alive[i]) steer(c, i);
  (void)w;
}

/* ── phase 4: move ────────────────────────────────────────────────────── */

/* Begins crossing the link whose start is the agent's last corner: the
 * corridor drops everything up to the link (it then starts on the far end's
 * polygon). False when the corridor does not hold it (it is replanned). */
static bool begin_link(crowd *c, agent *a) {
  cw__poly *path = corridor(c, a);
  cw__poly ref = a->corner_refs[a->ncorners - 1];
  int k = 1;
  while (k < a->npath && path[k] != ref) k++;
  if (k >= a->npath || k + 1 >= a->npath) return false;
  float s[3], e[3];
  uint32_t id = cw__nav_link_cross(c->nm, path[k - 1], ref, s, e);
  const cw_nav_link *l = cw__nav_link(c->nm, id);
  if (!id || !l) return false;
  memmove(path, path + k + 1, sizeof(cw__poly) * (size_t)(a->npath - k - 1));
  a->npath -= k + 1;
  memcpy(a->link_a, a->pos, sizeof a->link_a);
  memcpy(a->link_b, e, sizeof a->link_b);
  float dx = e[0] - a->pos[0], dy = e[1] - a->pos[1], dz = e[2] - a->pos[2];
  float len = sqrtf(dx * dx + dy * dy + dz * dz);
  a->link = id;
  a->event_link = id;
  a->link_t = 0;
  a->link_arc = l->arc;
  a->link_dur = a->p.manual_links ? 0 : l->duration > 0 ? l->duration : a->p.max_speed > 0 ? len / a->p.max_speed : 0;
  a->link_event = 1;
  a->ncorners = 0;
  /* while crossing, others see it moving straight at the far end */
  if (a->link_dur > 0) a->vel[0] = dx / a->link_dur, a->vel[1] = 0, a->vel[2] = dz / a->link_dur;
  else memset(a->vel, 0, sizeof a->vel);
  return true;
}

/* One step of a crossing: along the straight line from link_a to link_b,
 * raised by 4 arc t (1 - t); lands at t = 1. */
static void cross_link(crowd *c, agent *a) {
  if (a->p.manual_links) return; /* the game moves it and calls cw_crowd_finish_link */
  a->link_t = a->link_dur > 0 ? a->link_t + c->dt / a->link_dur : 1;
  if (a->link_t >= 1) {
    a->link_t = 1;
    memset(a->vel, 0, sizeof a->vel);
    land(c, a);
    return;
  }
  float t = a->link_t, up = 4 * a->link_arc * t * (1 - t);
  for (int k = 0; k < 3; k++) a->pos[k] = a->link_a[k] + (a->link_b[k] - a->link_a[k]) * t;
  a->pos[1] += up;
}

/* Near the start of the link its corners lead to: cross it. */
static void maybe_link(crowd *c, agent *a) {
  if (a->status != CW_CROWD_MOVING || a->ncorners <= 0 || !(a->corner_flags[a->ncorners - 1] & STRAIGHTPATH_LINK))
    return;
  const float *lc = &a->corners[(a->ncorners - 1) * 3];
  if (len2(lc[0] - a->pos[0], lc[2] - a->pos[2]) >= a->p.radius * LINK_TRIGGER || begin_link(c, a)) return;
  a->npath = a->npath ? 1 : 0;
  a->status = CW_CROWD_WAITING; /* the corridor lost the link: plan again */
  a->plan_seq = 0;              /* numbered after the phase, on the calling thread */
}

static void move_range(int b, int e, int w, void *ctx) {
  crowd *c = ctx;
  for (int i = b; i < e; i++) {
    if (!c->alive[i]) continue;
    agent *a = &c->agents[i];
    a->link_event = 0;
    if (a->link) {
      cross_link(c, a);
      continue;
    }
    memcpy(a->vel, a->nvel, sizeof a->vel);
    if (a->vel[0] == 0 && a->vel[2] == 0) {
      maybe_link(c, a);
      continue;
    }
    cw__poly *path = corridor(c, a);
    if (a->npath <= 0) continue;
    float to[3] = { a->pos[0] + a->vel[0] * c->dt, a->pos[1], a->pos[2] + a->vel[2] * c->dt };
    float out[3];
    cw__poly visited[MAX_VISITED];
    int nv = cw__nav_move_along(c->nm, path[0], a->pos, to, &c->filter, out, visited, MAX_VISITED);
    if (nv <= 0) continue;
    if (a->status == CW_CROWD_MOVING) {
      a->npath = merge_start_moved(path, a->npath, CORRIDOR_MAX, visited, nv);
    } else {
      path[0] = visited[nv - 1];
      a->npath = 1;
    }
    float hy;
    if (cw__nav_poly_height(c->nm, path[0], out, &hy)) out[1] = hy;
    /* the velocity the surface allowed */
    a->vel[0] = (out[0] - a->pos[0]) / c->dt;
    a->vel[2] = (out[2] - a->pos[2]) / c->dt;
    memcpy(a->pos, out, sizeof a->pos);
    maybe_link(c, a);
  }
  (void)w;
}

/* ── the step ─────────────────────────────────────────────────────────── */

/* Tiles changed (obstacles, rebuilds): agents whose corridor crosses a
 * polygon that is gone or now excluded replan; one standing on one re-snaps. */
static void revalidate(crowd *c) {
  for (int i = 0; i < c->max; i++) {
    agent *a = &c->agents[i];
    if (!(a->gen & 1) || a->npath <= 0) continue;
    cw__poly *path = corridor(c, a);
    if (!cw__nav_ref_ok(c->nm, path[0], &c->filter)) {
      float s[3];
      cw__poly ref;
      if (cw__nav_nearest_ref(c->nm, a->pos, &c->filter, &ref, s)) {
        memcpy(a->pos, s, sizeof a->pos);
        path[0] = ref;
      }
      a->npath = 1;
      if (a->status == CW_CROWD_MOVING || a->status == CW_CROWD_ARRIVED) {
        a->status = CW_CROWD_WAITING;
        a->plan_seq = ++c->seq;
      }
      continue;
    }
    if (a->status != CW_CROWD_MOVING) continue;
    for (int k = 1; k < a->npath; k++)
      if (!cw__nav_ref_ok(c->nm, path[k], &c->filter)) {
        a->npath = 1;
        a->status = CW_CROWD_WAITING;
        a->plan_seq = ++c->seq;
        break;
      }
    /* a target that was unreachable may be reachable now */
  }
  for (int i = 0; i < c->max; i++) {
    agent *a = &c->agents[i];
    if ((a->gen & 1) && (a->status == CW_CROWD_NO_PATH || (a->status == CW_CROWD_MOVING && a->partial)) &&
        a->target_ref && cw__nav_ref_ok(c->nm, a->target_ref, &c->filter)) {
      a->status = CW_CROWD_WAITING;
      a->plan_seq = ++c->seq;
    }
  }
}

void cw_crowd_step(cw_crowd h, float dt) {
  crowd *c = get(h);
  if (!c || !(dt > 0)) return;
  double t0 = cw__nav_now_ms(), t;
  c->dt = dt;
  uint32_t ver = cw__nav_version(c->nm);
  if (ver != c->nav_version) {
    c->nav_version = ver;
    revalidate(c);
  }
  /* 1. plan: the plan_per_step oldest waiting agents, by request order */
  int np = 0, K = c->plan_per_step;
  for (int i = 0; i < c->max; i++) {
    agent *a = &c->agents[i];
    if (!(a->gen & 1) || a->status != CW_CROWD_WAITING || a->link) continue; /* a crossing plans once it lands */
    int k;
    if (np < K) k = np++;
    else if (a->plan_seq < c->agents[c->plan_list[K - 1]].plan_seq) k = K - 1;
    else continue;
    while (k > 0 && a->plan_seq < c->agents[c->plan_list[k - 1]].plan_seq) {
      c->plan_list[k] = c->plan_list[k - 1];
      k--;
    }
    c->plan_list[k] = i;
  }
  cw_parallel_for(np, 1, plan_range, c);
  t = cw__nav_now_ms();
  c->stats.phase_ms[0] = (float)(t - t0);
  /* 2. the step's starting state, the grid and neighbours */
  int moving = 0, waiting = 0;
  for (int i = 0; i < c->max; i++) {
    agent *a = &c->agents[i];
    c->alive[i] = a->gen & 1;
    if (!c->alive[i]) continue;
    c->px[i] = a->pos[0], c->py[i] = a->pos[1], c->pz[i] = a->pos[2];
    c->vx[i] = a->vel[0], c->vz[i] = a->vel[2];
    c->rad[i] = a->p.radius, c->hgt[i] = a->p.height, c->pri[i] = a->p.avoidance_priority;
    c->avoids[i] = a->p.avoidance;
    moving += a->status == CW_CROWD_MOVING || a->status == CW_CROWD_VELOCITY || a->link;
    waiting += a->status == CW_CROWD_WAITING;
  }
  build_grid(c);
  cw_parallel_for(c->max, 64, neighbours_range, c);
  double t1 = cw__nav_now_ms();
  c->stats.phase_ms[1] = (float)(t1 - t);
  /* 3. steer and avoid */
  cw_parallel_for(c->max, 32, steer_range, c);
  double t2 = cw__nav_now_ms();
  c->stats.phase_ms[2] = (float)(t2 - t1);
  /* 4. move */
  cw_parallel_for(c->max, 64, move_range, c);
  /* in index order on this thread: agents whose corridor lost a link queue a plan; link events */
  for (int i = 0; i < c->max; i++) {
    agent *a = &c->agents[i];
    if (!(a->gen & 1)) continue;
    if (a->status == CW_CROWD_WAITING && a->plan_seq == 0) a->plan_seq = ++c->seq;
  }
  if (c->link_fn)
    for (int i = 0; i < c->max; i++) {
      agent *a = &c->agents[i];
      if (!(a->gen & 1) || !a->link_event) continue;
      const cw_nav_link *l = cw__nav_link(c->nm, a->event_link);
      cw_crowd_link_event ev = {
        .agent = CW_HANDLE_MAKE(cw_crowd_agent, (uint32_t)i + 1, a->gen),
        .link = a->event_link,
        .tag = l ? l->tag : 0,
        .start = { a->link_a[0], a->link_a[1], a->link_a[2] },
        .end = { a->link_b[0], a->link_b[1], a->link_b[2] },
        .duration = a->link_dur,
        .finished = a->link_event == 2,
      };
      c->link_fn((cw_crowd){ h.id }, &ev, c->link_user);
    }
  double t3 = cw__nav_now_ms();
  c->stats.phase_ms[3] = (float)(t3 - t2);
  int pairs = 0;
  for (int i = 0; i < c->max; i++)
    if (c->alive[i]) pairs += c->agents[i].nnb;
  c->stats.agents = c->live;
  c->stats.moving = moving;
  c->stats.waiting = waiting;
  c->stats.paths_planned = np;
  c->stats.neighbor_pairs = pairs;
  c->stats.step_ms = (float)(t3 - t0);
}

/* ── debug draw ───────────────────────────────────────────────────────── */

void cw_crowd_debug_draw(cw_crowd h, cw_phys_lines *out, uint32_t flags) {
  crowd *c = get(h);
  if (!c || !out) return;
  static const float ring[12][2] = { { 1, 0 }, { 0.8660254f, 0.5f }, { 0.5f, 0.8660254f }, { 0, 1 },
                                     { -0.5f, 0.8660254f }, { -0.8660254f, 0.5f }, { -1, 0 }, { -0.8660254f, -0.5f },
                                     { -0.5f, -0.8660254f }, { 0, -1 }, { 0.5f, -0.8660254f }, { 0.8660254f, -0.5f } };
  for (int i = 0; i < c->max; i++) {
    agent *a = &c->agents[i];
    if (!(a->gen & 1)) continue;
    float y = a->pos[1] + 0.05f;
    if (flags & CW_NAV_DRAW_AGENTS) {
      uint32_t col = a->status == CW_CROWD_MOVING ? 0x33ccff : a->status == CW_CROWD_ARRIVED ? 0x66ff66
                     : a->status == CW_CROWD_NO_PATH ? 0xff3333 : 0xcccccc;
      for (int k = 0; k < 12; k++) {
        const float *p0 = ring[k], *p1 = ring[(k + 1) % 12];
        float A[3] = { a->pos[0] + p0[0] * a->p.radius, y, a->pos[2] + p0[1] * a->p.radius };
        float B[3] = { a->pos[0] + p1[0] * a->p.radius, y, a->pos[2] + p1[1] * a->p.radius };
        cw_phys_lines_add(out, A, B, col);
      }
      float V[3] = { a->pos[0] + a->vel[0] * 0.5f, y, a->pos[2] + a->vel[2] * 0.5f };
      float P[3] = { a->pos[0], y, a->pos[2] };
      cw_phys_lines_add(out, P, V, 0xffff00);
    }
    if ((flags & CW_NAV_DRAW_PATHS) && a->status == CW_CROWD_MOVING && a->npath > 0) {
      t3_vec3 pts[32];
      float buf[32 * 3];
      int n = cw__nav_corners(c->nm, a->pos, a->goal, corridor(c, a), a->npath, buf, NULL, NULL, 32);
      for (int k = 0; k < n; k++) pts[k] = (t3_vec3){ buf[k * 3], buf[k * 3 + 1] + 0.1f, buf[k * 3 + 2] };
      cw_nav_debug_draw_path(out, pts, n, 0xff8800);
    }
  }
}

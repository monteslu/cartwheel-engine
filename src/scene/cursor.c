/* Cursors between targets (cartwheel/cursor.h): towers' hop, for any set of 2D or 3D spots and any number of
 * players, with the pointer picking through cartwheel/pick3d.h. */
#include "cartwheel/cursor.h"

#include <math.h>
#include <string.h>

#include "cartwheel/core.h"
#include "cartwheel/math.h"
#include "cartwheel/scene2d.h"

typedef struct cursor {
  int target;
  int8_t hx, hy; /* the way held last tick (-1, 0, 1 each) */
  int hold;      /* ticks it has been held */
  bool has_view;
  cw_view3d view;
} cursor;

struct cw_cursors {
  cw_cursor_desc d;
  t3_vec3 *pts;
  cw_node *nodes;
  bool use_nodes; /* the targets are nodes[] (else pts[]) */
  int n, cap;
  cw_vec2 *scr;   /* scratch: the targets on one player's screen */
  cw_cursor_allow_fn allow;
  void *user;
  cursor p[CW_CURSOR_MAX_PLAYERS];
};

static cw_cursor_desc defaults(const cw_cursor_desc *d) {
  cw_cursor_desc o = d ? *d : (cw_cursor_desc){ 0 };
  if (!(o.cone > 0)) o.cone = 1.75f;
  if (!(o.side_weight > 0)) o.side_weight = 2.2f;
  if (o.repeat_delay == 0) o.repeat_delay = 18;
  if (o.repeat_every <= 0) o.repeat_every = 9;
  if (!(o.deadzone > 0)) o.deadzone = 0.5f;
  if (!(o.pick_radius > 0)) o.pick_radius = 48;
  return o;
}

int cw_cursor_hop_select(const cw_vec2 *pts, int n, int from, cw_vec2 dir, const cw_cursor_desc *dd, cw_cursor_allow_fn allow,
                         int player, void *user) {
  if (!pts || from < 0 || from >= n) return -1;
  cw_cursor_desc d = defaults(dd);
  float l = sqrtf(dir.x * dir.x + dir.y * dir.y);
  if (!(l > 0)) return -1;
  float ux = dir.x / l, uy = dir.y / l;
  cw_vec2 c = pts[from];
  int best = -1;
  float bs = INFINITY;
  for (int k = 0; k < n; k++) {
    if (k == from || (allow && !allow(player, k, user))) continue;
    float vx = pts[k].x - c.x, vy = pts[k].y - c.y;
    float along = vx * ux + vy * uy, side = fabsf(vx * uy - vy * ux);
    if (!(along > 0) || side > along * d.cone) continue;
    float sc = along + side * d.side_weight;
    if (sc < bs) bs = sc, best = k;
  }
  return best;
}

cw_cursors *cw_cursors_new(const cw_cursor_desc *d) {
  cw_cursors *c = cw_calloc(1, sizeof *c, CW_MEM_GAME);
  c->d = defaults(d);
  for (int i = 0; i < CW_CURSOR_MAX_PLAYERS; i++) c->p[i].target = -1;
  return c;
}

void cw_cursors_free(cw_cursors *c) {
  if (!c) return;
  cw_free(c->pts), cw_free(c->nodes), cw_free(c->scr), cw_free(c);
}

static void reserve(cw_cursors *c, int n) {
  if (n <= c->cap) return;
  c->pts = cw_realloc(c->pts, sizeof *c->pts * (size_t)n, CW_MEM_GAME);
  c->scr = cw_realloc(c->scr, sizeof *c->scr * (size_t)n, CW_MEM_GAME);
  c->nodes = cw_realloc(c->nodes, sizeof *c->nodes * (size_t)n, CW_MEM_GAME);
  c->cap = n;
}
static void clamp_targets(cw_cursors *c) {
  for (int i = 0; i < CW_CURSOR_MAX_PLAYERS; i++)
    if (c->p[i].target >= c->n) c->p[i].target = -1;
}

void cw_cursors_set_points(cw_cursors *c, const t3_vec3 *pts, int n) {
  if (!c) return;
  n = n > 0 && pts ? n : 0;
  reserve(c, n);
  if (n) memcpy(c->pts, pts, sizeof *pts * (size_t)n);
  c->n = n, c->use_nodes = false;
  clamp_targets(c);
}

void cw_cursors_set_nodes(cw_cursors *c, const cw_node *nodes, int n) {
  if (!c) return;
  n = n > 0 && nodes ? n : 0;
  reserve(c, n);
  if (n) memcpy(c->nodes, nodes, sizeof *nodes * (size_t)n);
  c->n = n, c->use_nodes = true;
  clamp_targets(c);
}

int cw_cursors_count(const cw_cursors *c) { return c ? c->n : 0; }
void cw_cursors_set_allow(cw_cursors *c, cw_cursor_allow_fn fn, void *user) {
  if (c) c->allow = fn, c->user = user;
}
void cw_cursors_set_view(cw_cursors *c, int player, const cw_view3d *v) {
  if (!c || player < 0 || player >= CW_CURSOR_MAX_PLAYERS) return;
  c->p[player].has_view = v != NULL;
  if (v) c->p[player].view = *v;
}

int cw_cursors_target(const cw_cursors *c, int player) {
  return c && player >= 0 && player < CW_CURSOR_MAX_PLAYERS ? c->p[player].target : -1;
}
void cw_cursors_place(cw_cursors *c, int player, int target) {
  if (!c || player < 0 || player >= CW_CURSOR_MAX_PLAYERS) return;
  c->p[player].target = target >= 0 && target < c->n ? target : -1;
}

/* a target's world place (a node's, read now) and whether it is 2D */
static t3_vec3 place(const cw_cursors *c, int k, bool *two_d) {
  *two_d = false;
  if (!c->use_nodes) {
    *two_d = !c->d.points_xz;
    return c->pts[k];
  }
  cw_node n = c->nodes[k];
  if (cw_node_is(n, "Node3D")) return cw_node3d_global_position(n);
  *two_d = true;
  cw_vec2 p = cw_node2d_global_position(n);
  return t3_v3(p.x, p.y, 0);
}

static cw_vec2 screen_of(const cw_cursors *c, int player, int k) {
  bool two_d;
  t3_vec3 w = place(c, k, &two_d);
  const cursor *p = &c->p[player];
  if (p->has_view && (!two_d || !c->use_nodes) && cw_node_valid(p->view.camera)) return cw_camera3d_world_to_screen(p->view.camera, p->view.rect, w, NULL);
  return two_d ? (cw_vec2){ w.x, w.y } : (cw_vec2){ w.x, w.z };
}

static void screens(cw_cursors *c, int player) {
  for (int k = 0; k < c->n; k++) c->scr[k] = screen_of(c, player, k);
}

cw_vec2 cw_cursors_screen(const cw_cursors *c, int player, int target) {
  if (!c || player < 0 || player >= CW_CURSOR_MAX_PLAYERS || target < 0 || target >= c->n) return (cw_vec2){ 0, 0 };
  return screen_of(c, player, target);
}

static bool allowed(const cw_cursors *c, int player, int k) { return !c->allow || c->allow(player, k, c->user); }

/* a cursor with nowhere to stand goes to the allowed target nearest where it was (the first allowed, from none) */
static void settle(cw_cursors *c, int player) {
  cursor *p = &c->p[player];
  if (p->target >= 0 && p->target < c->n && allowed(c, player, p->target)) return;
  screens(c, player);
  cw_vec2 from = p->target >= 0 && p->target < c->n ? c->scr[p->target] : (cw_vec2){ 0, 0 };
  bool have = p->target >= 0 && p->target < c->n;
  int best = -1;
  float bd = INFINITY;
  for (int k = 0; k < c->n; k++) {
    if (!allowed(c, player, k)) continue;
    if (!have) { best = k; break; }
    float dx = c->scr[k].x - from.x, dy = c->scr[k].y - from.y, d = dx * dx + dy * dy;
    if (d < bd) bd = d, best = k;
  }
  p->target = best;
}

/* a stick or direction snapped to eight ways (-1, 0, 1 each), nothing inside the dead zone */
static void ways(float dz, cw_vec2 v, int *dx, int *dy) {
  *dx = *dy = 0;
  float l = sqrtf(v.x * v.x + v.y * v.y);
  if (!(l >= dz) || !(l > 0)) return;
  /* the nearest of eight: each axis counts when it is at least tan(22.5 degrees) of the other */
  const float t = 0.41421356f;
  float ax = fabsf(v.x), ay = fabsf(v.y);
  if (ax >= ay * t) *dx = v.x > 0 ? 1 : -1;
  if (ay >= ax * t) *dy = v.y > 0 ? 1 : -1;
}

static int hop_way(cw_cursors *c, int player, int dx, int dy) {
  cursor *p = &c->p[player];
  if (p->target < 0 || (!dx && !dy)) return -1;
  screens(c, player);
  int k = cw_cursor_hop_select(c->scr, c->n, p->target, (cw_vec2){ (float)dx, (float)dy }, &c->d, c->allow, player, c->user);
  if (k >= 0) p->target = k;
  return k;
}

int cw_cursors_hop(cw_cursors *c, int player, cw_vec2 dir) {
  if (!c || player < 0 || player >= CW_CURSOR_MAX_PLAYERS) return -1;
  settle(c, player);
  int dx, dy;
  ways(1e-6f, dir, &dx, &dy);
  return hop_way(c, player, dx, dy);
}

int cw_cursors_step(cw_cursors *c, int player, cw_vec2 stick) {
  if (!c || player < 0 || player >= CW_CURSOR_MAX_PLAYERS) return -1;
  cursor *p = &c->p[player];
  settle(c, player);
  int dx, dy;
  ways(c->d.deadzone, stick, &dx, &dy);
  if (dx != p->hx || dy != p->hy) p->hold = 0;
  else if (p->hold < 1 << 30) p->hold++;
  p->hx = (int8_t)dx, p->hy = (int8_t)dy;
  if (!dx && !dy) return -1;
  bool go = p->hold == 0 ||
            (c->d.repeat_delay > 0 && p->hold >= c->d.repeat_delay && (p->hold - c->d.repeat_delay) % c->d.repeat_every == 0);
  return go ? hop_way(c, player, dx, dy) : -1;
}

int cw_cursors_point(cw_cursors *c, int player, cw_tree *t, cw_vec2 at) {
  if (!c || player < 0 || player >= CW_CURSOR_MAX_PLAYERS || !c->n) return -1;
  cursor *p = &c->p[player];
  /* a 3D hit on a target node, through the player's view (else the view under the point) */
  if (c->use_nodes && t) {
    cw_pick3d_hit h;
    cw_pick3d_filter f = { .layers = c->d.pick_layers };
    int got = 0;
    if (p->has_view && cw_node_valid(p->view.camera)) {
      cw_rect r = p->view.rect;
      if (at.x >= r.x && at.y >= r.y && at.x < r.x + r.w && at.y < r.y + r.h) got = cw_pick3d_screen(p->view.camera, r, at, &f, &h, 1);
    } else got = cw_pick3d_canvas(t, at, &f, &h, 1, NULL);
    for (cw_node n = got ? h.node : (cw_node){ 0 }; cw_node_valid(n); n = cw_node_parent(n))
      for (int k = 0; k < c->n; k++)
        if (c->nodes[k].id == n.id) {
          if (!allowed(c, player, k)) return -1;
          p->target = k;
          return k;
        }
  }
  screens(c, player);
  int best = -1;
  float bd = c->d.pick_radius * c->d.pick_radius;
  for (int k = 0; k < c->n; k++) {
    if (!allowed(c, player, k)) continue;
    float dx = c->scr[k].x - at.x, dy = c->scr[k].y - at.y, d = dx * dx + dy * dy;
    if (d <= bd) bd = d, best = k;
  }
  if (best >= 0) p->target = best;
  return best;
}

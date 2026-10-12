/* The authoring bot (native dev tool, never part of a gate run): with
 * ACTION2D_BOT=<file.json> the game plays itself through the platform's raw
 * input filter, deciding each frame's pad 0 / mouse state from the game state
 * the previous frame left (menus by a fixed prologue, rooms by breadth-first
 * paths over the tile grid, swings when an enemy is in reach), and records
 * that input as a tools/run-cart.mjs input script. The gates then REPLAY the
 * recording on every host; the bot only saves writing hundreds of frame
 * numbers by hand. tests/action2d-script.mjs turns the recording into the pad
 * and keyboard scripts, their controls and games/action2d/script.inc. */
#include "game.h"

#if !CW_CART
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct { int frame; uint32_t buttons; float lx, ly; int mx, my; uint32_t mb; } entry;
static CW_ARRAY(entry) rec;
static entry cur, last;
static const char *out_path;
static int title_at = -1, victory_at = -1, pause_at = -1, written;
static float stuck_x, stuck_y;
static int stuck_since, nudge_until, nudge_dir;

enum { B_SOUTH = 1u << CW_PAD_SOUTH, B_NORTH = 1u << CW_PAD_NORTH, B_START = 1u << CW_PAD_START,
       B_UP = 1u << CW_PAD_UP, B_DOWN = 1u << CW_PAD_DOWN, B_LEFT = 1u << CW_PAD_LEFT, B_RIGHT = 1u << CW_PAD_RIGHT };

/* the menu prologue, frames after the title appeared: a mouse click on
 * OPTIONS, the sliders, the attack rebinding (captured: pad north), back, start */
static const struct { int at; uint32_t tap; int mx, my, mb; } PROLOGUE[] = {
  { 40, 0, 480, 322, 0 }, { 42, 0, 480, 322, 1 }, { 45, 0, 480, 322, 0 }, { 50, 0, 950, 530, 0 },
  { 70, B_RIGHT, -1, 0, 0 }, { 80, B_RIGHT, -1, 0, 0 }, { 90, B_DOWN, -1, 0, 0 }, { 100, B_LEFT, -1, 0, 0 },
  { 110, B_DOWN, -1, 0, 0 }, { 120, B_SOUTH, -1, 0, 0 }, { 140, B_NORTH, -1, 0, 0 }, { 160, B_DOWN, -1, 0, 0 },
  { 170, B_SOUTH, -1, 0, 0 }, { 190, B_UP, -1, 0, 0 }, { 200, B_SOUTH, -1, 0, 0 },
};

static void write_out(int frame) {
  FILE *f = fopen(out_path, "w");
  if (!f) { cw_error("bot: cannot write %s", out_path); return; }
  static const char *const names[] = { "south", "east", "west", "north", "back", "guide", "start", "l3", "r3", "l", "r",
                                       "up", "down", "left", "right" };
  fprintf(f, "[\n");
  for (int i = 0; i < rec.count; i++) {
    entry *e = &rec.data[i];
    fprintf(f, "  { \"frame\": %d", e->frame);
    if (e->buttons) {
      fprintf(f, ", \"buttons\": [");
      int k = 0;
      for (int b = 0; b < 15; b++)
        if (e->buttons & (1u << b)) fprintf(f, "%s\"%s\"", k++ ? ", " : "", names[b]);
      fprintf(f, "]");
    }
    if (e->lx != 0 || e->ly != 0) fprintf(f, ", \"axes\": { \"lx\": %g, \"ly\": %g }", (double)e->lx, (double)e->ly);
    if (e->mx >= 0) fprintf(f, ", \"mouse\": { \"x\": %d, \"y\": %d%s }", e->mx, e->my, e->mb ? ", \"buttons\": [\"left\"]" : "");
    fprintf(f, " }%s\n", i + 1 < rec.count ? "," : "");
  }
  fprintf(f, "]\n");
  fclose(f);
  cw_info("bot: wrote %d entries to %s; victory at frame %d, recording ends at %d", rec.count, out_path, victory_at, frame);
  written = 1;
}

/* breadth-first over the room's tile grid (8-neighbour, no corner cutting):
 * the first step from (sx, sy) towards (tx, ty); false when unreachable */
static bool path_step(int sx, int sy, int tx, int ty, int *nx, int *ny) {
  int w, h;
  actors_room_size(&w, &h);
  if (w <= 0 || sx < 0 || sy < 0 || sx >= w || sy >= h) return false;
  if (sx == tx && sy == ty) { *nx = tx; *ny = ty; return true; }
  int n = w * h;
  int *from = malloc(sizeof(int) * (size_t)n), *queue = malloc(sizeof(int) * (size_t)n);
  for (int i = 0; i < n; i++) from[i] = -1;
  int qh = 0, qt = 0, start = sy * w + sx, goal = ty * w + tx;
  from[start] = start;
  queue[qt++] = start;
  static const int DX[] = { 1, -1, 0, 0, 1, 1, -1, -1 }, DY[] = { 0, 0, 1, -1, 1, -1, 1, -1 };
  while (qh < qt && from[goal] < 0) {
    int c = queue[qh++], cx = c % w, cy = c / w;
    for (int k = 0; k < 8; k++) {
      int x = cx + DX[k], y = cy + DY[k];
      if (x < 0 || y < 0 || x >= w || y >= h || from[y * w + x] >= 0) continue;
      if (actors_cell_solid(x, y)) continue;
      if (k >= 4 && (actors_cell_solid(cx + DX[k], cy) || actors_cell_solid(cx, cy + DY[k]))) continue;
      from[y * w + x] = c;
      queue[qt++] = y * w + x;
    }
  }
  bool ok = from[goal] >= 0;
  if (ok) {
    int c = goal;
    while (from[c] != start) c = from[c];
    *nx = c % w;
    *ny = c / w;
  }
  free(from);
  free(queue);
  return ok;
}

static void steer_to(cw_vec2 hero, cw_vec2 target, int frame) {
  cw_vec2 feet = cw_v2(hero.x, hero.y + 3), goal = target;
  int nx, ny;
  if (path_step((int)(feet.x / 16), (int)(feet.y / 16), (int)(target.x / 16), (int)(target.y / 16), &nx, &ny) &&
      !(nx == (int)(target.x / 16) && ny == (int)(target.y / 16)))
    goal = cw_v2((float)nx * 16 + 8, (float)ny * 16 + 8 - 3);
  float dx = goal.x - hero.x, dy = goal.y - hero.y;
  cur.lx = dx > 2 ? 1.0f : dx < -2 ? -1.0f : 0;
  cur.ly = dy > 2 ? 1.0f : dy < -2 ? -1.0f : 0;
  if (frame < nudge_until) {
    if (nudge_dir & 1) cur.lx = nudge_dir & 2 ? 1.0f : -1.0f;
    else cur.ly = nudge_dir & 2 ? 1.0f : -1.0f;
  }
}

static cw_node nearest(const char *group, cw_vec2 from, bool enemies, float *radius) {
  cw_node *list, best = { 0 };
  int n = cw_tree_group(cw_scene_app_tree(), group, &list);
  float bd = 1e30f;
  for (int i = 0; i < n; i++) {
    if (cw_node_parent(list[i]).id != G.room_node.id) continue;
    float r = 0;
    if (enemies && !actors_enemy_alive(list[i], &r)) continue;
    cw_vec2 p = cw_node2d_position(list[i]);
    float d = (p.x - from.x) * (p.x - from.x) + (p.y - from.y) * (p.y - from.y);
    if (d < bd) { bd = d; best = list[i]; if (radius) *radius = r; }
  }
  return best;
}

static void play_frame(int frame) {
  cur.buttons = 0;
  cur.lx = cur.ly = 0;
  if (G.state != ST_PLAY || !cw_node_valid(G.hero) || G.hp <= 0) return;
  cw_vec2 h = cw_node2d_position(G.hero);
  /* stuck: nudge sideways for a moment */
  if ((h.x - stuck_x) * (h.x - stuck_x) + (h.y - stuck_y) * (h.y - stuck_y) > 4) {
    stuck_x = h.x, stuck_y = h.y, stuck_since = frame;
  } else if (frame - stuck_since > 40 && frame >= nudge_until) {
    nudge_until = frame + 16;
    nudge_dir = (frame / 7) & 3;
    stuck_since = frame;
  }
  /* dodge: an orb coming at us within 44 px pushes us off its line */
  cw_node *shots;
  int ns = cw_tree_group(cw_scene_app_tree(), "shots", &shots);
  for (int i = 0; i < ns; i++) {
    cw_value v;
    if (!cw_node_get_prop(shots[i], "velocity", &v)) continue;
    cw_vec2 o = cw_node2d_position(shots[i]), rel = cw_v2(h.x - o.x, h.y - o.y), vel = cw_v2(v.v[0], v.v[1]);
    float along = rel.x * vel.x + rel.y * vel.y, vv = vel.x * vel.x + vel.y * vel.y;
    if (along <= 0 || vv <= 0 || rel.x * rel.x + rel.y * rel.y > 44 * 44) continue;
    float t = along / vv;
    cw_vec2 miss = cw_v2(rel.x - vel.x * t, rel.y - vel.y * t); /* closest approach offset */
    if (miss.x * miss.x + miss.y * miss.y > 10 * 10) continue;
    if (miss.x * miss.x + miss.y * miss.y < 0.01f) miss = cw_v2(-vel.y, vel.x);
    cur.lx = miss.x > 0.5f ? 1.0f : miss.x < -0.5f ? -1.0f : 0;
    cur.ly = miss.y > 0.5f ? 1.0f : miss.y < -0.5f ? -1.0f : 0;
    return;
  }
  /* low on health: a heart first */
  if (G.hp <= 2) {
    cw_node *list;
    int n = cw_tree_group(cw_scene_app_tree(), "pickups", &list);
    for (int i = 0; i < n; i++) {
      cw_value k;
      if (cw_node_parent(list[i]).id != G.room_node.id || !cw_node_get_prop(list[i], "kind", &k) || k.i != 0) continue;
      steer_to(h, cw_node2d_position(list[i]), frame);
      return;
    }
  }
  float er = 0;
  cw_node e = nearest("enemies", h, true, &er);
  if (cw_node_valid(e) && cw_node_is(e, "Boss")) {
    int timer, st = actors_enemy_state(e, &timer);
    cw_vec2 p = cw_node2d_position(e);
    if (st == 2 /* winding up */ || st == 4 /* roaring */ || st == 0) {
      /* back off to 56 px while it winds up */
      float dx = h.x - p.x, dy = h.y - p.y;
      if (dx * dx + dy * dy < 56 * 56) {
        cur.lx = dx > 0 ? 1.0f : -1.0f;
        cur.ly = dy > 0 ? 1.0f : -1.0f;
        if (frame < nudge_until) cur.lx = -cur.lx;
      }
      return;
    }
  }
  if (cw_node_valid(e)) {
    cw_vec2 p = cw_node2d_position(e);
    float dx = p.x - h.x, dy = p.y - h.y, d2 = dx * dx + dy * dy, reach = 14 + er;
    if (d2 < reach * reach) {
      /* face it along the dominant axis and swing */
      if (dx * dx > dy * dy) cur.lx = dx > 0 ? 1.0f : -1.0f;
      else cur.ly = dy > 0 ? 1.0f : -1.0f;
      if (actors_hero_cooldown() == 0 && !(last.buttons & B_NORTH)) cur.buttons |= B_NORTH;
    } else {
      steer_to(h, p, frame);
    }
    return;
  }
  cw_node pick = (G.hp < HERO_MAX_HP) ? nearest("pickups", h, false, NULL) : (cw_node){ 0 };
  if (cw_node_valid(pick) && !cw_node_is(pick, "Pickup")) pick = (cw_node){ 0 };
  if (!cw_node_valid(pick)) {
    /* gems only: hearts stay while health is full */
    cw_node *list;
    int n = cw_tree_group(cw_scene_app_tree(), "pickups", &list);
    float bd = 1e30f;
    for (int i = 0; i < n; i++) {
      cw_value k;
      if (cw_node_parent(list[i]).id != G.room_node.id || !cw_node_get_prop(list[i], "kind", &k)) continue;
      if (k.i == 0 && G.hp >= HERO_MAX_HP) continue;
      cw_vec2 p = cw_node2d_position(list[i]);
      float d = (p.x - h.x) * (p.x - h.x) + (p.y - h.y) * (p.y - h.y);
      if (d < bd) { bd = d; pick = list[i]; }
    }
  }
  if (cw_node_valid(pick)) { steer_to(h, cw_node2d_position(pick), frame); return; }
  cw_vec2 door;
  if (actors_door(&door)) steer_to(h, cw_v2(door.x, door.y + 2), frame);
}

static void bot_filter(cw_raw_input *in, uint64_t fidx, void *user) {
  (void)user;
  int frame = (int)fidx + 1; /* the script's 1-based frame number */
  if (written) return;
  if (title_at < 0 && G.state == ST_TITLE) title_at = frame;
  cur.mx = last.mx;
  cur.my = last.my;
  cur.mb = 0;
  if (title_at >= 0 && G.state != ST_PLAY && G.state != ST_PAUSE && G.state != ST_VICTORY && G.state != ST_TRANSITION) {
    cur.buttons = 0;
    cur.lx = cur.ly = 0;
    for (size_t i = 0; i < CW_ARRAY_LEN(PROLOGUE); i++) {
      int at = title_at + PROLOGUE[i].at;
      if (PROLOGUE[i].mx >= 0 && frame >= at) { cur.mx = PROLOGUE[i].mx; cur.my = PROLOGUE[i].my; cur.mb = (uint32_t)PROLOGUE[i].mb; }
      if (PROLOGUE[i].tap && frame >= at && frame < at + 3) cur.buttons = PROLOGUE[i].tap;
    }
  } else if (G.state == ST_PLAY || G.state == ST_PAUSE || G.state == ST_TRANSITION) {
    /* one pause after the first kill: start, then resume with the menu */
    if (pause_at < 0 && G.kills >= 1 && G.state == ST_PLAY) pause_at = frame;
    if (pause_at >= 0 && frame < pause_at + 60) {
      cur.buttons = frame < pause_at + 3 ? B_START : (frame >= pause_at + 45 && frame < pause_at + 48) ? B_SOUTH : 0;
      cur.lx = cur.ly = 0;
    } else {
      play_frame(frame);
    }
  } else {
    cur.buttons = 0;
    cur.lx = cur.ly = 0;
  }
  if (G.state == ST_VICTORY && victory_at < 0) victory_at = frame;
  if ((victory_at >= 0 && frame > victory_at + 90) || frame > 30000) { write_out(frame); return; }
  if (rec.count == 0 || cur.buttons != last.buttons || cur.lx != last.lx || cur.ly != last.ly || cur.mx != last.mx ||
      cur.my != last.my || cur.mb != last.mb) {
    cur.frame = frame;
    cw_array_push(&rec, cur);
    last = cur;
  }
  memset(&in->pads[0], 0, sizeof in->pads[0]);
  in->pads[0].connected = true;
  in->pads[0].buttons = cur.buttons;
  in->pads[0].axes[CW_AXIS_LX] = cur.lx;
  in->pads[0].axes[CW_AXIS_LY] = cur.ly;
  if (cur.mx >= 0) {
    in->mouse_present = true;
    in->mouse_x = (float)cur.mx;
    in->mouse_y = (float)cur.my;
    in->mouse_buttons = cur.mb;
  }
}

void bot_install(const char *path) {
  out_path = path;
  last.mx = cur.mx = -1;
  cw_set_raw_input_filter(bot_filter, NULL);
}
#else
void bot_install(const char *path) { (void)path; }
#endif

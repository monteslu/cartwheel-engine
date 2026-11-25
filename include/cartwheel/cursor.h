/* cartwheel cursors between targets: a cursor per player that hops between arbitrary spots (the squares of a
 * board, the cards of a market, the build pads of a map, the cities of a network) by stick direction, and that a
 * mouse or finger moves by pointing.
 *
 *   cw_cursors *cur = cw_cursors_new(NULL);                          (the defaults below)
 *   cw_cursors_set_nodes(cur, pieces, n);                            (or cw_cursors_set_points: 2D or 3D points)
 *   cw_cursors_set_view(cur, p, &(cw_view3d){ cam[p], cw_shell_player_rect(p) });   (each player's picture)
 *   ... in fixed_update, per player:
 *   cw_cursors_step(cur, p, cw_input_vector(p, left, right, up, down, CW_DEADZONE_RADIAL, 0.2f));
 *   if (clicked) cw_cursors_point(cur, 0, tree, canvas_px);
 *   int at = cw_cursors_target(cur, p);
 *
 * The hop: of the targets the player may stand on (cw_cursors_set_allow), the one lying most nearly the way held
 * on the player's own screen. A target counts when it lies ahead (along the held way, past the cursor) and its
 * sideways offset is at most `cone` times its distance ahead (1.75: about 60 degrees either side); of those the
 * lowest `along + side_weight x sideways` wins (2.2), so a near target a little off the line beats a far one dead
 * ahead, and a target straight ahead beats one as near but off to the side. Nothing ahead: the cursor stays.
 * Held ways are the eight directions (a stick past `deadzone` snaps to the nearest), so a diagonal on a square grid
 * goes diagonally. A hop happens on the tick the way is first held, then again every `repeat_every` ticks once it
 * has been held `repeat_delay` ticks (18 and 9: a quarter second, then about seven a second); changing the way
 * starts over. This is towers' cursor (even-more-cartwheel-examples games/towers) with its numbers as defaults.
 *
 * Directions are screen directions: with a view (a Camera3D and the rectangle its picture fills) a 3D target is
 * projected through that player's camera, so up on the stick is up on that player's picture in split screen too,
 * whatever way the camera faces (points with a view are 3D points). Without a view, a Node3D target is read on the
 * board plane seen from above (x right, z toward the bottom of the screen: a camera at +Z looking down at the
 * board), a Node2D target and a point as canvas or 2D world coordinates (x right, y down), and a point on the board
 * plane too with points_xz.
 *
 * Every player (up to CW_CURSOR_MAX_PLAYERS) has their own cursor; any number move at once, and two may stand on
 * one target (block that with the allow callback). Everything is + - * / and sqrt on the points (the projection
 * through a view uses cw_sincos), so cursors hop the same on every host and may live in a deterministic
 * simulation: keep the cw_cursors in the game state, or keep the target index and call cw_cursor_hop_select. Main
 * thread only. Guide: docs/guides/input.md#cursors-between-targets. */
#ifndef CW_CURSOR_H
#define CW_CURSOR_H

#include "pick3d.h"

#ifdef __cplusplus
extern "C" {
#endif

#define CW_CURSOR_MAX_PLAYERS 8

typedef struct cw_cursor_desc {
  float cone;        /* a target counts when its sideways offset <= cone x its distance ahead (0 = 1.75) */
  float side_weight; /* score = ahead + side_weight x sideways, lowest wins (0 = 2.2) */
  int repeat_delay;  /* ticks a way is held before the hop repeats (0 = 18; -1 = never repeats) */
  int repeat_every;  /* then a hop every this many ticks (0 = 9) */
  float deadzone;    /* stick length below which no way is held (0 = 0.5) */
  float pick_radius; /* a pointer picks the nearest target within this many pixels when no 3D hit decides (0 = 48) */
  bool points_xz;    /* points without a view are 3D board points read as (x, z), not 2D points (x, y) */
  uint32_t pick_layers; /* the render layers a pointer's 3D hit tests (cw_pick3d_filter.layers; 0 = every layer):
                         * leave the cursor markers' layer out */
} cw_cursor_desc;

/* May this player stand on that target (their own pieces, an empty square)? NULL = every target. */
typedef bool (*cw_cursor_allow_fn)(int player, int target, void *user);

/* The stateless hop: from target `from` (its place pts[from]), the target the screen direction dir picks among
 * pts[0..n-1] (screen space, y down), or -1 when none lies that way. dir need not be unit length; it is used as
 * given (snap it to eight ways first for towers' feel; cw_cursors_step does). d NULL = the defaults. */
int cw_cursor_hop_select(const cw_vec2 *pts, int n, int from, cw_vec2 dir, const cw_cursor_desc *d, cw_cursor_allow_fn allow,
                         int player, void *user);

typedef struct cw_cursors cw_cursors;

cw_cursors *cw_cursors_new(const cw_cursor_desc *d); /* NULL = the defaults; every cursor starts on no target */
void cw_cursors_free(cw_cursors *c); /* NULL is fine */
/* The targets as points (2D ones: z 0), copied. A cursor whose target index is now out of range goes to no target. */
void cw_cursors_set_points(cw_cursors *c, const t3_vec3 *pts, int n);
/* The targets as nodes (Node3D: its global position; Node2D: its global position, z 0), read at each step, hop and
 * point, so pieces that move carry their targets along; a pointer hit on a node's meshes (or on anything under it)
 * picks it. Copied. */
void cw_cursors_set_nodes(cw_cursors *c, const cw_node *nodes, int n);
int cw_cursors_count(const cw_cursors *c); /* the targets set */
void cw_cursors_set_allow(cw_cursors *c, cw_cursor_allow_fn fn, void *user); /* which targets each player may stand on; NULL = all */
/* Which way is which for a player: the camera and the rectangle its picture fills (cw_canvas_views3d,
 * cw_shell_player_rect). NULL: no view (the board plane, as above). */
void cw_cursors_set_view(cw_cursors *c, int player, const cw_view3d *view);

int cw_cursors_target(const cw_cursors *c, int player); /* -1 none */
/* Put a player's cursor on a target (-1: none), whatever the allow callback says. */
void cw_cursors_place(cw_cursors *c, int player, int target);
/* Once per tick with the player's stick (x right, y down, as cw_input_vector): hops on the tick a way is first held
 * and repeats while it is held. A cursor on no target, or on one it may no longer stand on, first goes to the
 * nearest target it may. Returns the target hopped to, -1 when it did not move. */
int cw_cursors_step(cw_cursors *c, int player, cw_vec2 stick);
/* One hop now the way dir points (screen space, snapped to eight ways); the new target or -1 when it did not move. */
int cw_cursors_hop(cw_cursors *c, int player, cw_vec2 dir);
/* A pointer (canvas pixels) picks a target for this player: with node targets, the node a 3D hit lands on
 * (cw_pick3d_canvas, the player's view's camera) when it is a target or under one; else the target whose place on
 * the player's screen is nearest the point, within pick_radius. The cursor moves there; returns it, -1 when the
 * point picked nothing the player may stand on (the cursor stays). t: the tree the 3D hit is tested in (NULL: no
 * 3D test). */
int cw_cursors_point(cw_cursors *c, int player, cw_tree *t, cw_vec2 canvas_px);
/* Where a target lies on that player's screen (the view's pixels, or the plane coordinates without a view). */
cw_vec2 cw_cursors_screen(const cw_cursors *c, int player, int target);

#ifdef __cplusplus
}
#endif
#endif

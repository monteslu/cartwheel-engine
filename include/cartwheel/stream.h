/* cartwheel world streaming: a big world cut into square cells that load,
 * activate, deactivate and unload around the player while the game runs,
 * with no loading screen.
 *
 * ── cells ──────────────────────────────────────────────────────────────
 * The world is a cells_x x cells_z grid of cell_size metre squares on the XZ
 * plane, cell (0, 0)'s min corner at `origin`. Cell (cx, cz) has id
 * cz * cells_x + cx and, when path_format is set, one file of its own
 * (snprintf(path, path_format, cx, cz)): one file per cell, so a lazily
 * loaded web cart waits on one cell at most. A ".zst" / ".lz4" suffix
 * decompresses on the job pool (cartwheel/resource.h). Every cell needs its
 * file; an empty cell is a small file.
 *
 * A cell moves through:
 *   NONE      not wanted
 *   LOADING   its file is loading (cw_res_load_async_ex, nearest first)
 *   LOADED    on_load ran (on the main thread, from cw_stream_frame, at
 *             most prepare_per_frame cells a frame): what is only drawn
 *             (meshes, terrain, scatter) exists and shows
 *   ACTIVE    on_activate ran (from cw_stream_tick): what the simulation
 *             sees (bodies, navmesh tiles, entities) exists
 * and back down: on_deactivate (tick) then on_unload (tick), after which
 * the file is released (cw_res_set_residency_budget can keep it cached).
 *
 * ── sources and radii ──────────────────────────────────────────────────
 * Sources (the player, the camera, a remote player) say where cells are
 * needed. A cell is wanted LOADED while its square comes within
 * load_radius of a source (horizontal distance, the nearest point of the
 * square), and ACTIVE within active_radius (<= load_radius). Hysteresis: a
 * cell leaves a set only once it is farther than radius + hysteresis, so a
 * player on a border does not make cells thrash. Loads start nearest first
 * (source priority, then distance, then id), at most max_requests_per_tick
 * a tick.
 *
 * ── determinism ────────────────────────────────────────────────────────
 * What the simulation sees changes only in cw_stream_tick (call it from
 * fixed_update), and only from the sources' positions: cells activate and
 * deactivate in cell id order on the tick their distance says, whatever the
 * disk, the network or the job pool did. When a cell must activate and its
 * file has not arrived (or on_load has not run), the tick waits for it: a
 * STALL, counted in the stats (stalls, stall_ms). That keeps every host
 * identical; size load_radius from your IO so stalls never happen
 * (load_radius - active_radius is the distance the source travels while a
 * cell loads). activation_hash folds every (tick, cell, on/off) event: equal
 * hashes mean equal activation histories.
 *
 * ── large worlds: the floating origin ──────────────────────────────────
 * Positions far from the origin lose float precision (at 8 km a float
 * steps 1 mm; at 100 km, 8 mm, and the camera shakes). The stream keeps a
 * LOCAL origin in double precision: everything the game simulates and draws
 * lives in local float coordinates near it, and world positions are
 * cw_dvec3. When the first source gets farther than rebase_distance from
 * the local origin (horizontally), cw_stream_tick moves the origin by whole
 * cells toward it (shift = a whole number of cells, so a power-of-two
 * cell_size keeps every shifted coordinate exact where precision allows),
 * then calls on_rebase with the shift: move your bodies
 * (cw_world3d_shift_origin) and objects by -shift. Cells get on_cell_rebase
 * with their new local origin. It happens on a tick the sources decide, so
 * it is deterministic. Rendering is camera-relative as a result: the camera
 * and everything near it are near the local origin.
 *
 * Threads: main thread only. Callbacks run on the main thread.
 * Memory: CW_MEM_GAME for the grid (cells_x * cells_z small records). */
#ifndef CW_STREAM_H
#define CW_STREAM_H

#include "base.h"
#include "resource.h"
#include "three.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct cw_dvec3 { double x, y, z; } cw_dvec3;

typedef struct cw_stream cw_stream;

typedef enum {
  CW_CELL_NONE,
  CW_CELL_LOADING,
  CW_CELL_LOADED,
  CW_CELL_ACTIVE,
} cw_cell_state;

/* What a callback gets about a cell. */
typedef struct cw_stream_cell {
  int cx, cz;
  int id;              /* cz * cells_x + cx */
  cw_dvec3 origin;     /* world position of the cell's min corner (y = the stream's origin.y) */
  t3_vec3 local;       /* the same in local coordinates (cw_stream_to_local) */
  cw_res res;          /* its file: READY (or FAILED: the log says why); null without path_format */
  void *data;          /* what on_load returned */
} cw_stream_cell;

typedef struct cw_stream_desc {
  int cells_x, cells_z;      /* grid size (each at most 4096) */
  double cell_size;          /* metres; a power of two keeps origin shifts exact: 512 */
  cw_dvec3 origin;           /* world position of cell (0, 0)'s min corner */
  const char *path_format;   /* "world/c_%d_%d.cell.zst" (cx, cz); NULL = cells without files */
  cw_res_type cell_type;     /* 0 = from the path's extension */
  float load_radius;         /* metres: 1500 */
  float active_radius;       /* metres (<= load_radius): 600 */
  float hysteresis;          /* metres: 0 = cell_size / 4 */
  int max_requests_per_tick; /* 0 = 8 */
  int prepare_per_frame;     /* on_load calls per cw_stream_frame: 0 = 1 */
  int res_lane;              /* cw_res lane of cell files: 0 */
  double rebase_distance;    /* metres: 0 = never move the origin */
  /* callbacks (main thread; any may be NULL) */
  void *(*on_load)(cw_stream *s, const cw_stream_cell *c, void *user);   /* returns the cell's data */
  void (*on_unload)(cw_stream *s, const cw_stream_cell *c, void *user);
  void (*on_activate)(cw_stream *s, const cw_stream_cell *c, void *user);
  void (*on_deactivate)(cw_stream *s, const cw_stream_cell *c, void *user);
  void (*on_rebase)(cw_stream *s, t3_vec3 shift, void *user);            /* before the cells' on_cell_rebase */
  void (*on_cell_rebase)(cw_stream *s, const cw_stream_cell *c, void *user); /* every LOADED / ACTIVE cell, id order */
  /* optional: is on_load's work done (a terrain set's background build: cw_terrain_set_ready)?
   * With wait, finish it now and return true. A cell counts as LOADED only once this says so, and a
   * cell that must activate while still preparing makes the tick wait (a stall). Polled each frame. */
  bool (*cell_ready)(cw_stream *s, const cw_stream_cell *c, bool wait, void *user);
  void *user;
} cw_stream_desc;
/* The defaults noted above (grid and callbacks empty). */
cw_stream_desc cw_stream_default_desc(void);

/* NULL (logged) for an empty or oversized grid or radii out of order. */
cw_stream *cw_stream_create(const cw_stream_desc *d);
/* Deactivates and unloads every cell (id order), releases their files. */
void cw_stream_destroy(cw_stream *s);

/* ── sources ────────────────────────────────────────────────────────── */
/* A source at world position p; returns its index. Higher priority loads
 * first. The first source added drives the origin. At most 8. */
int cw_stream_source_add(cw_stream *s, cw_dvec3 p, int priority);
/* Moves source src to world position p (call before cw_stream_tick). */
void cw_stream_source_move(cw_stream *s, int src, cw_dvec3 p);
/* The same from local coordinates (what physics and the scene use). */
void cw_stream_source_move_local(cw_stream *s, int src, t3_vec3 p);

/* ── per tick and per frame ─────────────────────────────────────────── */
/* fixed_update: rebase when due, then decide the wanted sets from the
 * sources, deactivate and unload what left them, start loads, and activate
 * (in id order) what entered the active set, waiting for any cell not loaded
 * yet. Cost: O(cells within the radii) plus the callbacks. */
void cw_stream_tick(cw_stream *s);
/* Every frame (before drawing): on_load for cells whose file is ready, at
 * most prepare_per_frame, nearest first. */
void cw_stream_frame(cw_stream *s);

/* ── queries ────────────────────────────────────────────────────────── */
cw_cell_state cw_stream_cell_state(const cw_stream *s, int cx, int cz); /* NONE outside the grid */
void *cw_stream_cell_data(const cw_stream *s, int cx, int cz);          /* on_load's result, NULL when not LOADED */
/* The cell containing a world position (false outside the grid). */
bool cw_stream_cell_at(const cw_stream *s, cw_dvec3 p, int *cx, int *cz);
/* The desc it was made with, defaults filled in (hysteresis, request and prepare limits). */
const cw_stream_desc *cw_stream_get_desc(const cw_stream *s);

/* ── coordinates ────────────────────────────────────────────────────── */
cw_dvec3 cw_stream_origin(const cw_stream *s); /* world position of local (0, 0, 0) */
/* World (double) to local (float, relative to the local origin): what physics and three.c use. */
t3_vec3 cw_stream_to_local(const cw_stream *s, cw_dvec3 world);
/* Local to world: exact for every local float (a double holds the sum). */
cw_dvec3 cw_stream_to_world(const cw_stream *s, t3_vec3 local);

typedef struct cw_stream_stats {
  int loading, loaded, active;      /* cells now (active cells are loaded too) */
  uint64_t loads, activations, deactivations, unloads; /* so far */
  int start_cells;                  /* cells the first tick activated (waiting for them: the start position's load) */
  double start_ms;                  /* how long that took (wall clock) */
  int stalls;                       /* later activations that had to wait for their cell (should stay 0) */
  double stall_ms, stall_ms_max;    /* wall clock spent waiting (reported, never fed back) */
  double prepare_ms_max;            /* the longest on_load so far (wall clock) */
  int rebases;
  uint64_t activation_hash;         /* FNV-1a 64 over (tick, cell id, on/off) events */
  uint64_t ticks;
} cw_stream_stats;
/* The counters now (zero for NULL). Cost: O(live cells). */
void cw_stream_get_stats(const cw_stream *s, cw_stream_stats *out);

#ifdef __cplusplus
}
#endif
#endif

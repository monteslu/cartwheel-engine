/* Play-in-editor, the editor side: shared by this module's files only.
 *
 *   session.c          the play backend (cwed_play_register) on cartwheel/remote.h:
 *                      transport choice, launch/relaunch/stop, pause/step, the
 *                      crash report, the play.* commands beyond the core's
 *   game_panel.c       Game panel: status, crash banner, controls, frame preview
 *   remote_tree.c      Remote panel: the running game's tree (tree + changes <rev>)
 *   live_inspector.c   Live panel: the inspected remote node's properties
 *                      (props, then polled get), live edits and "keep in scene"
 *   json.c             a small JSON reader for the game's replies */
#ifndef CWED_PLAY_H
#define CWED_PLAY_H

#include "cartwheel/remote.h"
#include "cwed/editor.h"

/* ── JSON ─────────────────────────────────────────────────────────────── */
typedef enum { PJ_NULL, PJ_BOOL, PJ_NUM, PJ_STR, PJ_ARR, PJ_OBJ } pj_type;
typedef struct pj {
  pj_type t;
  double n;
  bool b;
  char *s;
  int count;
  struct pj **items;
  char **keys; /* objects */
} pj;
pj *pj_parse(const char *text); /* NULL on bad input */
void pj_free(pj *v);
pj *pj_get(const pj *o, const char *key);
pj *pj_at(const pj *a, int i);
const char *pj_str(const pj *o, const char *key);
double pj_num(const pj *o, const char *key, double def);
bool pj_bool(const pj *o, const char *key, bool def);

/* ── the session ──────────────────────────────────────────────────────── */
bool pl_available(void);            /* false in the cart editor: no transport */
const char *pl_unavailable_why(void);
bool pl_running(void);              /* a game answers requests */
bool pl_active(void);               /* a session exists (running, crashed, exited) */
int pl_generation(void);            /* +1 per launch / relaunch / stop: views resync */
const char *pl_transport(void);     /* "native", "romdev" or "none" */
const char *pl_scene(void);         /* the scene playing ("" when none) */
const char *pl_status(void);        /* one line: state, transport, tick, paused */
const char *pl_crash(void);         /* the last crash/exit report, NULL when none */
bool pl_is_paused(void);
long long pl_tick(void);
bool pl_preview_default(void);      /* frames in the Game panel by default (no game window) */
bool pl_sync(void);                 /* test runs: requests complete inside the frame that sent them */
int pl_poll_frames(void);

/* async request; cb runs inside the editor frame (dropped after a relaunch) */
typedef void (*pl_cb)(const cw_remote_reply *r, void *user);
uint32_t pl_send(const char *request, pl_cb cb, void *user);
/* blocking request (commands): the reply JSON (malloc'd) or NULL with *err (malloc'd) */
char *pl_call(const char *request, char **err);
/* wait for an async request (pl_send id) to complete */
bool pl_wait(uint32_t id, int timeout_ms);
/* a value or name as one debug-channel token ("..." with \" and \\ when needed) */
const char *pl_token(const char *text);

/* the open document of the played scene, and a remote node path in it
 * ("/root/Level/Ball" -> "Ball"; NULL when the node is not in that document) */
cwed_doc *pl_doc(void);
const char *pl_doc_path(const char *remote_path);

/* the remote node the Remote tree and the Live inspector show */
void pl_inspect(int uid);
int pl_inspected(void);
/* the inspected node's game path: a relaunch inspects the same node again */
void pl_inspect_path(const char *path);

/* panels */
void pl_game_init(void);
void pl_game_frame(void);
void pl_game_show_png(const void *png, size_t len, const char *what);
void pl_game_preview_drain(void); /* finish a live-preview capture in flight (one capture at a time) */
void pl_game_widgets(cwed_json *j);
void pl_tree_init(void);
void pl_tree_frame(void);
void pl_tree_json(cwed_json *j);
void pl_tree_widgets(cwed_json *j);
void pl_insp_init(void);
void pl_insp_frame(void);
void pl_insp_widgets(cwed_json *j);

/* a rect as JSON members of an object: "name":{"x":..,"y":..,"w":..,"h":..} */
void pl_rect_json(cwed_json *j, const char *name, cw_rect r);

#endif

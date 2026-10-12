/* cartwheel remote: the EDITOR side of play-in-editor (M4.3).
 *
 * Launches a game under edit and talks to it over the debug channel
 * (cartwheel/debug.h), the same text requests and JSON replies romdev's
 * wasm({op:'command'}) uses. The game never runs in the editor's address
 * space, so a crashing game cannot take the editor down: the editor sees the
 * crash as a state change and relaunches.
 *
 * Transports (same API, same requests):
 *   CW_REMOTE_NATIVE  the game executable as a child process. The editor
 *                     makes a socketpair; the child inherits one end and gets
 *                     CW_REMOTE=fd:N (the channel turns on even without
 *                     .debug). Exits and crashes (signals) are detected with
 *                     waitpid; the child's stdout/stderr come back as log
 *                     lines. Linux only: on Linux the child also dies with
 *                     the editor (PR_SET_PDEATHSIG, plus EOF on the socket
 *                     quits it); macOS works the same but without the
 *                     death signal; Windows is not implemented yet.
 *   CW_REMOTE_ROMDEV  the game's .wasc in romdev (HTTP to its tool route,
 *                     default http://127.0.0.1:7331, one session per
 *                     cw_remote): loadMedia, then each request is
 *                     wasm({op:'command'}). romdev runs a cart only when told,
 *                     so a worker thread keeps the game's frames flowing at
 *                     60 per wall-clock second while it is not paused. A
 *                     wasm trap or an evicted host is reported as a crash /
 *                     exit like a native one.
 * An editor that is itself a CART has no transport: a cart cannot open a
 * socket or spawn anything, and wasmcart's wc_peer_* (the only network ABI)
 * needs a host that registers a peer to the game, which no host does today
 * (romdev has no wc_peer support). cw_remote_launch fails there and says so.
 *
 * Threading: one cw_remote belongs to one thread (the editor's frame loop);
 * the romdev transport's worker is internal.
 *
 * Use from a frame loop:
 *
 *   cw_remote *g = cw_remote_launch(&(cw_remote_desc){ .transport = CW_REMOTE_NATIVE,
 *                                                      .exe = "build/native/samples/platformer2d",
 *                                                      .start_paused = true });
 *   cw_remote_send(g, "tree", 0, on_tree, ui);     // async; on_tree runs inside pump
 *   ... every frame: cw_remote_pump(g);
 *   if (cw_remote_state_of(g) == CW_REMOTE_CRASHED) show(cw_remote_status(g)), cw_remote_relaunch(g);
 *
 * The request language is the debug channel's (`help` lists a game's
 * commands). The play-in-editor set: status, pause [1|0], resume,
 * step <ticks>, tree [node] [depth], node <node>, changes <revision>,
 * get <node> <prop...>, set <node> <prop> <value>, props, methods, call,
 * emit, spawn, free, change_scene, reload_scene, reload <res path...>,
 * screenshot <world|ui|debug|all> [scale] + screenshot_get, record ...,
 * stats, phys, quit. Nodes are addressed as #uid (stable) or by path. */
#ifndef CW_REMOTE_H
#define CW_REMOTE_H

#include "base.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct cw_remote cw_remote;

typedef enum { CW_REMOTE_NATIVE, CW_REMOTE_ROMDEV } cw_remote_transport;

typedef struct cw_remote_desc {
  cw_remote_transport transport;
  /* native */
  const char *exe;               /* the game executable */
  const char *const *args;       /* extra argv entries, NULL-terminated (may be NULL) */
  const char *const *env;        /* extra "NAME=value" entries, NULL-terminated (may be NULL) */
  const char *cwd;               /* working directory (NULL = the editor's) */
  bool headless;                 /* SDL_VIDEODRIVER=offscreen, SDL_AUDIODRIVER=dummy (tests, agents) */
  /* romdev */
  const char *cart;              /* path of the .wasc */
  const char *romdev_url;        /* NULL = $ROMDEV_URL, else http://127.0.0.1:$ROMDEV_PORT (default 7331) */
  const char *session;           /* romdev session key (NULL = a fresh one per launch) */
  /* both */
  bool start_paused;             /* the debugger's pause after frame 1 (tick 1 on both transports) */
  uint32_t seed;                 /* cw_host_seed (native CW_SEED, romdev deterministicSeed); 0 = 1 */
  int timeout_ms;                /* default per-request timeout (0 = 10000) */
  int launch_timeout_ms;         /* until the game answers its first request (0 = 20000) */
} cw_remote_desc;

typedef enum {
  CW_REMOTE_STARTING, /* launched, not answering yet */
  CW_REMOTE_RUNNING,  /* answering requests (paused or not) */
  CW_REMOTE_EXITED,   /* ended on its own (quit, exit code, romdev host evicted) */
  CW_REMOTE_CRASHED,  /* a signal / abort / wasm trap: cw_remote_status says which */
  CW_REMOTE_FAILED,   /* could not launch or connect (cw_remote_status says why) */
  CW_REMOTE_CLOSED,   /* cw_remote_stop */
} cw_remote_state;

/* Launch the game. Returns NULL only when d is unusable (no exe / cart) or
 * on a cart build; a game that fails to start comes back in state FAILED
 * with the reason in cw_remote_status. Blocks until the game answers
 * (`status`) or launch_timeout_ms passes. cw_remote_last_error() explains a
 * NULL. */
cw_remote *cw_remote_launch(const cw_remote_desc *d);
const char *cw_remote_last_error(void); /* why the last cw_remote_launch returned NULL */
/* Stop the game (quit request, then a kill after a moment) and free r.
 * Pending requests complete with an error. */
void cw_remote_close(cw_remote *r);
/* Stop the game if it still runs and start it again from the same desc.
 * Requests in flight fail. true when the new game answers. */
bool cw_remote_relaunch(cw_remote *r);
/* Stop the game, keep r (state CLOSED; relaunch restarts it). */
void cw_remote_stop(cw_remote *r);
/* Kill the game without asking (SIGKILL natively; romdev: the host is shut
 * down). For crash-isolation tests; the state becomes CRASHED. */
void cw_remote_kill(cw_remote *r);

cw_remote_state cw_remote_state_of(cw_remote *r); /* where the game is; pump updates it */
const char *cw_remote_state_name(cw_remote_state s); /* "running", "crashed", ... */
/* One line on the game's state: "running (pid 1234)", "crashed: signal 11
 * (SIGSEGV)", "exited with code 0", "romdev: unreachable (wasm trap)", ... */
const char *cw_remote_status(cw_remote *r);
int cw_remote_pid(cw_remote *r); /* native child pid, 0 otherwise */
int cw_remote_launch_count(cw_remote *r); /* 1 after launch, +1 per relaunch */

/* ── requests ─────────────────────────────────────────────────────────
 * Requests are answered in order of arrival by the game, one or more per
 * frame. A reply is JSON text: {"error": "..."} for a request the game
 * refused (the message names the call that fixes it), anything else for a
 * success. Transport failures (timeout, the game died) have ok = false and
 * error set, and json NULL. */
typedef struct cw_remote_reply {
  uint32_t id;
  bool ok;             /* the game answered with a non-error reply */
  const char *json;    /* the game's reply (also for an {"error"} reply); NULL on a transport failure */
  size_t len;
  const char *error;   /* why ok is false: the game's error text, or the transport's */
} cw_remote_reply;
typedef void (*cw_remote_cb)(cw_remote *r, const cw_remote_reply *reply, void *user);

/* Queue a request (debug-channel text, under 4 KiB for romdev). Returns its
 * id (> 0), or 0 when the game is not running (cb is not called then).
 * timeout_ms 0 = the desc's default. cb (may be NULL) runs inside
 * cw_remote_pump / cw_remote_wait on the calling thread; the reply's
 * strings are valid during the callback only. */
uint32_t cw_remote_send(cw_remote *r, const char *request, int timeout_ms, cw_remote_cb cb, void *user);
/* Deliver what has arrived: replies (callbacks), timeouts, exits/crashes,
 * log lines. Never blocks. Returns how many replies it delivered. Call once
 * per editor frame. */
int cw_remote_pump(cw_remote *r);
/* Pump until request `id` completes or timeout_ms passes. true when it
 * completed (successfully or not: the callback or cw_remote_take says). */
bool cw_remote_wait(cw_remote *r, uint32_t id, int timeout_ms);

/* Blocking convenience (tools, tests, agents): send, wait, return the reply
 * JSON (malloc'd; free it) or NULL with the reason in *error_out (malloc'd,
 * may be NULL). A game-side {"error"} reply returns NULL with that text. */
char *cw_remote_call(cw_remote *r, const char *request, int timeout_ms, char **error_out);
/* Same, with printf formatting. */
char *cw_remote_callf(cw_remote *r, char **error_out, const char *fmt, ...) __attribute__((format(printf, 3, 4)));

/* ── events ────────────────────────────────────────────────────────── */
typedef enum {
  CW_REMOTE_EV_STATE, /* text: the new status line (cw_remote_state_of says which) */
  CW_REMOTE_EV_LOG,   /* text: one line the game printed (stdout/stderr natively) */
  CW_REMOTE_EV_HELLO, /* text: the game's hello event JSON (native: pid, protocol) */
} cw_remote_event;
typedef void (*cw_remote_event_fn)(cw_remote *r, cw_remote_event ev, const char *text, void *user);
void cw_remote_on_event(cw_remote *r, cw_remote_event_fn fn, void *user); /* one listener; NULL removes it */
/* The last n log lines joined with '\n' (a crash report's context); valid
 * until the next pump. */
const char *cw_remote_log_tail(cw_remote *r, int n);

/* ── helpers built on requests ───────────────────────────────────────── */

/* Step `ticks` fixed ticks of a (now) paused game and wait for them. Splits
 * big counts for romdev's per-command frame cap. Returns the tick reached,
 * or -1 (error_out says why). */
long long cw_remote_step(cw_remote *r, uint32_t ticks, int timeout_ms, char **error_out);
/* Capture one frame with only `layers` ("world", "ui", "debug", "all", or
 * joined with '+') at 1/scale size and write it as PNG to png_path (NULL:
 * only *png_out). *png_out (may be NULL) receives the bytes (malloc'd) and
 * *png_len their count. false with the reason in *error_out. */
bool cw_remote_screenshot(cw_remote *r, const char *layers, int scale, const char *png_path, void **png_out,
                          size_t *png_len, int timeout_ms, char **error_out);

/* Tiny JSON readers for replies (flat lookups by key, first match at any
 * depth; enough for status/step/set replies without a JSON library).
 * Return false when the key is missing. */
bool cw_remote_json_number(const char *json, const char *key, double *out);
bool cw_remote_json_bool(const char *json, const char *key, bool *out); /* true / false only */
/* a string value, unescaped, malloc'd; NULL when missing */
char *cw_remote_json_string(const char *json, const char *key);

#ifdef __cplusplus
}
#endif
#endif

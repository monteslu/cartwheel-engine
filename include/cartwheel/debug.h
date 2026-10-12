/* Named debug state: values a game (or the engine) exposes BY NAME to romdev,
 * the editor and tests.
 *
 * Carts publish the registry through the wasmcart debug ABI (WC_FLAG_DEBUG +
 * wc_debug_state(), read pull-only by the host); set cw_app_desc.debug to
 * opt in. Natively, CW_DEBUG_DUMP=path writes every field as JSON when the
 * app exits, so a native run and a cart run can be compared field by field.
 *
 * Register in init() (the table is built once, on first read). Pointers must
 * stay valid for the app's lifetime. */
#ifndef CW_DEBUG_H
#define CW_DEBUG_H

#include "base.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
  CW_DBG_U8, CW_DBG_I8, CW_DBG_U16, CW_DBG_I16, CW_DBG_U32, CW_DBG_I32, CW_DBG_F32, CW_DBG_F64, CW_DBG_BYTES
} cw_debug_type; /* same numbering as the wasmcart WC_DBG_* types */

/* Registers (or re-points) a named value: len elements of type at ptr (len 0 = 1). name and ptr are kept,
 * not copied, so both must outlive the app. Up to 512 fields; main thread only. */
void cw_debug_field(const char *name, void *ptr, cw_debug_type type, uint32_t len);
#define cw_debug_i32(name, p) cw_debug_field(name, p, CW_DBG_I32, 1)
#define cw_debug_u32(name, p) cw_debug_field(name, p, CW_DBG_U32, 1)
#define cw_debug_f32(name, p) cw_debug_field(name, p, CW_DBG_F32, 1)
#define cw_debug_f64(name, p) cw_debug_field(name, p, CW_DBG_F64, 1)
#define cw_debug_f32_array(name, p, n) cw_debug_field(name, p, CW_DBG_F32, n)
#define cw_debug_bytes(name, p, n) cw_debug_field(name, p, CW_DBG_BYTES, n)

/* Write every field as one JSON object ({"name": value | [values] | "hex"}). */
void cw_debug_dump_json(const char *path);

/* ── the debug channel ────────────────────────────────────────────────
 * A request/reply mailbox in debug fields (dbg.cmd, dbg.cmd_seq, dbg.reply,
 * dbg.reply_seq, dbg.reply_len): a host writes a text request and bumps
 * dbg.cmd_seq; at the start of the next frame the app answers into
 * dbg.reply, so each request costs one frame of the running game (freeze it
 * first with `pause` when that matters). romdev's wasm({op:'command', text})
 * speaks it. Natively the same requests also travel over a socket
 * (CW_REMOTE=fd:N | tcp:HOST:PORT: frames of [u32 length][u32 id][text],
 * replies carry the request's id, any number in flight), which is what
 * cartwheel/remote.h (play-in-editor) launches games with; CW_REMOTE turns
 * the channel on even without .debug. CW_DEBUG_CMDS="cmd1;cmd2" runs
 * commands at the end of a CW_FRAMES run and prints the replies (tests).
 * Requests are "name arg arg..."; quote an arg containing spaces with "",
 * whole (`"a b"`) or in part (`key="a b"` is the one argument `key=a b`;
 * inside quotes \" and \\ escape). Parentheses keep their spaces and quotes:
 * vec3(1, 2, 3) and node("/root/x") are one argument each.
 * Replies are JSON; a refusal is exactly {"error":"..."} and names the call
 * that fixes it. `help` lists a game's commands. Enabled with .debug = true.
 * Built in: help, status, stats, pause [1|0], resume, step [ticks],
 * screenshot <layers> [scale], screenshot_get, fields [name...] (debug field
 * values), record, quit, debug_crash, vfs;
 * scene apps add tree, node, changes, get, set, props, methods, call, emit,
 * spawn, free, save, types, group, tree_pause, change_scene, reload_scene;
 * modules add res, reload, phys, impulse, anim*, animtree*, pipeline,
 * lighting, environment. */
typedef struct cw_reply cw_reply;
typedef void (*cw_debug_cmd_fn)(int argc, const char **argv, cw_reply *r);
/* Adds a channel command, or replaces one (built-ins too) with the same name. name and usage are kept,
 * not copied (use literals). fn runs on the main thread at the start of a frame. */
void cw_debug_command(const char *name, const char *usage, cw_debug_cmd_fn fn);
/* reply builders (JSON) */
void cw_reply_raw(cw_reply *r, const char *json);
/* Appends formatted raw JSON (any length). */
void cw_reply_printf(cw_reply *r, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
void cw_reply_str(cw_reply *r, const char *s); /* a JSON string, escaped */
/* Discards anything written so far and replies {"error":"msg"}; msg cut at 1023 bytes. */
void cw_reply_error(cw_reply *r, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
/* Deferred replies: a command whose answer needs frames to pass (step,
 * screenshot) calls cw_reply_defer instead of writing a reply. `wait` runs at
 * the start of every following frame until it writes the reply and returns
 * true; `state` is passed back (and is the command's to free inside wait).
 * Over the mailbox the host keeps stepping frames until the reply lands
 * (romdev's wasm({op:'command', maxFrames})); over a socket other requests
 * are answered meanwhile. cw_debug_run (CW_DEBUG_CMDS) cannot wait: it
 * answers {"deferred":true}. */
typedef bool (*cw_debug_wait_fn)(cw_reply *r, void *state);
void cw_reply_defer(cw_reply *r, cw_debug_wait_fn wait, void *state); /* answer later (above) */
/* Run one request now (used by the channel and by native tests). Returns a
 * cw_alloc'd JSON reply. */
char *cw_debug_run(const char *request);
void cw__debug_channel_init(void); /* platform: when .debug (or CW_REMOTE natively) */
void cw__debug_channel_poll(void); /* platform: start of every frame */
void cw__debug_channel_app_ready(void); /* platform: after the app's init (answers requests held for its commands) */
void cw__debug_frame_end(void);    /* platform: after the frame rendered (captures) */
bool cw__debug_remote_requested(void); /* platform: CW_REMOTE is set (native) */

/* ── the debugger's pause (play-in-editor) ─────────────────────────────
 * Freezes the whole simulation, unlike the game's own tree pause
 * (cw_tree_set_paused, which a pause menu uses and which PAUSE_ALWAYS nodes
 * ignore): while paused no fixed tick, update or input frame runs, but frames
 * still render, window events are still read and the debug channel is still
 * answered, so an editor can inspect and edit a frozen game.
 * cw_debug_step(n) runs exactly n more ticks, one per frame, each frame
 * exactly as an unpaused 1/60 s frame runs it (one fixed tick, update with
 * dt = fixed dt, render), so pausing at tick T and stepping n lands in the
 * same state as running to T + n. Channel commands: pause, resume, step,
 * status. Natively CW_START_PAUSED=1 pauses after the first frame (the frame
 * a cart's host runs on load), so both start at tick 1. */
void cw_debug_pause(bool paused);
bool cw_debug_paused(void); /* the debugger's pause is on (steps may be running) */
void cw_debug_step(uint32_t ticks);    /* pauses too; adds to the steps left */
uint32_t cw_debug_steps_left(void); /* ticks cw_debug_step still owes */

/* ── render layers (screenshot by layer) ───────────────────────────────
 * WORLD: every pipeline phase before OVERLAY (3D scene, sky, effects, post)
 * and the tree's 2D canvas pass; UI: the canvas overlay passes (Control
 * nodes) and the app's render callback (HUDs); DEBUG: OVERLAY passes with
 * order >= 500 (debug draw; 1000 is the convention). A frame drawn without
 * WORLD starts from black (transparent where the window surface has alpha;
 * natively and in carts it usually has none, so captures read alpha 1). */
enum { CW_LAYER_WORLD = 1, CW_LAYER_UI = 2, CW_LAYER_DEBUG = 4, CW_LAYER_ALL = 7 };
void cw_render_set_layers(uint32_t mask); /* CW_LAYER_ALL by default */
uint32_t cw_render_layers(void); /* the CW_LAYER_* mask frames render with now */

#ifdef __cplusplus
}
#endif
#endif

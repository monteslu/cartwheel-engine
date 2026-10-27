/* cartwheel base: fixed-width types, asserts, logging, handles.
 *
 * Every public header includes this. Nothing here touches SDL, GL or the
 * wasmcart ABI. */
#ifndef CW_BASE_H
#define CW_BASE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#if defined(__wasm__)
#define CW_CART 1
#else
#define CW_CART 0
#endif

/* ── logging ──────────────────────────────────────────────────────────
 * Native: stderr. Carts: wc_log, which debug-capable hosts (romdev) capture
 * into the frame-stamped event trace. */
typedef enum { CW_LOG_DEBUG, CW_LOG_INFO, CW_LOG_WARN, CW_LOG_ERROR } cw_log_level;
/* Formats (truncated at 1023 bytes), prints via the platform, then calls each listener. Thread-safe. */
void cw_log(cw_log_level level, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
#define cw_info(...) cw_log(CW_LOG_INFO, __VA_ARGS__)
#define cw_warn(...) cw_log(CW_LOG_WARN, __VA_ARGS__)
#define cw_error(...) cw_log(CW_LOG_ERROR, __VA_ARGS__)
/* Log listeners see every line after the platform printed it (an editor's
 * Output panel, a test collecting errors). Called on the thread that logged,
 * so a listener must be thread-safe, quick, and must not log. Up to 8;
 * returns an id for cw_log_remove_listener, or -1 when full. Register and
 * remove on the main thread. */
typedef void (*cw_log_listener)(cw_log_level level, const char *msg, void *user);
int cw_log_add_listener(cw_log_listener fn, void *user); /* see above; -1 when all 8 slots are taken */
/* Bad id: no-op. Returns once calls running on other threads have finished,
 * so what the listener uses may be freed after it (from inside a listener
 * it does not wait for that listener's own call). */
void cw_log_remove_listener(int id);

/* ── asserts ──────────────────────────────────────────────────────────
 * CW_ASSERT is compiled in unless NDEBUG; its message should name the fix.
 * cw_fatal always aborts (a cart traps, which every host reports). */
#ifdef __cplusplus
[[noreturn]]
#else
_Noreturn
#endif
/* Logs "file:line: message" as an error and aborts. Listeners are NOT called. Use CW_FATAL for file/line. */
void cw_fatal(const char *file, int line, const char *fmt, ...) __attribute__((format(printf, 3, 4)));
#define CW_FATAL(...) cw_fatal(__FILE__, __LINE__, __VA_ARGS__)
#ifdef NDEBUG
#define CW_ASSERT(cond, ...) ((void)0)
#else
#define CW_ASSERT(cond, ...) ((cond) ? (void)0 : cw_fatal(__FILE__, __LINE__, "assert(" #cond ") " __VA_ARGS__))
#endif
#define CW_STATIC_ASSERT(cond, msg) _Static_assert(cond, msg)

#define CW_ARRAY_LEN(a) (sizeof(a) / sizeof((a)[0]))
#define CW_MIN(a, b) ((a) < (b) ? (a) : (b))
#define CW_MAX(a, b) ((a) > (b) ? (a) : (b))
#define CW_CLAMP(x, lo, hi) CW_MIN(CW_MAX(x, lo), hi)

/* ── handles ──────────────────────────────────────────────────────────
 * Generational handles: {index, generation} packed in 64 bits. 0 is the null
 * handle (generation 0 is never issued). Each kind is its own struct type so
 * a node handle cannot be passed where a resource handle is expected. */
#define CW_HANDLE_TYPE(name) typedef struct name { uint64_t id; } name
#define CW_HANDLE_INDEX(h) ((uint32_t)((h).id & 0xffffffffu))
#define CW_HANDLE_GEN(h) ((uint32_t)((h).id >> 32))
#define CW_HANDLE_MAKE(T, index, gen) ((T){ ((uint64_t)(gen) << 32) | (uint32_t)(index) })
#define CW_HANDLE_IS_NULL(h) ((h).id == 0)
/* A live handle's generation is never 0, so a non-null handle whose top half is 0 was cut to 32 bits somewhere
 * (a handle kept in a uint32_t or an int). Every lookup that sees one reports it (cw_handle_report_truncated)
 * instead of answering as if the handle were merely stale. Keep the handle struct itself, never its .id cast
 * down; for an array index use the module's own slot accessor (cw_crowd_agent_slot) or CW_HANDLE_INDEX. */
#define CW_HANDLE_IS_TRUNCATED(h) ((h).id != 0 && ((h).id >> 32) == 0)

#ifdef __cplusplus
}
#endif
#endif

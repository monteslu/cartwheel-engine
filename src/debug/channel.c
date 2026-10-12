/* The debug channel: requests in, JSON replies out.
 *
 * Two transports carry the same requests:
 *   - the MAILBOX: debug fields (dbg.cmd / dbg.cmd_seq in, dbg.reply /
 *     dbg.reply_seq / dbg.reply_len out), one request in flight, read
 *     pull-only by a cart's host (romdev's wasm({op:'command'}));
 *   - a SOCKET (native only): CW_REMOTE=fd:N (an inherited socket, what
 *     cartwheel/remote.h launches games with) or CW_REMOTE=tcp:HOST:PORT.
 *     Frames are [u32 length LE][u32 id LE][payload]; a reply carries its
 *     request's id; id 0 is an event from the game ({"event":"hello",...}).
 *     Any number of requests may be in flight. EOF quits the game: an editor
 *     that goes away takes its game along.
 * Both are served at the start of every frame (cw__debug_channel_poll),
 * paused or not. */
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>

#include "cartwheel/core.h"
#include "cartwheel/debug.h"
#include "cartwheel/gl.h"
#include "cartwheel/jobs.h"
#include "cartwheel/platform.h"
#include "cartwheel/vfs.h"
bool cw__wgpu_readback(unsigned char **px, int *w, int *h); /* platform/gpu.c */
#include "stb_image_write.h"

#if !CW_CART && defined(_WIN32)
#include <errno.h>
#include <winsock2.h>
#include <ws2tcpip.h>
#include <process.h>
#elif !CW_CART
#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

#define CMD_CAP 4096
#define REPLY_CAP (256 * 1024)

static char cmd_buf[CMD_CAP];
static char reply_buf[REPLY_CAP];
static uint32_t cmd_seq, reply_seq, reply_len, handled_seq;
static bool inited;
/* the app's init has run (cw__debug_channel_app_ready): until then the app's
 * own commands are not registered yet, so a request naming a command nobody
 * has registered waits for the init instead of failing as unknown */
static bool app_ready;

struct cw_reply {
  char *buf;
  size_t len, cap;
  bool error;
  cw_debug_wait_fn wait; /* cw_reply_defer */
  void *wait_state;
};

typedef struct { const char *name, *usage; cw_debug_cmd_fn fn; } cmd_entry;
static CW_ARRAY(cmd_entry) cmds;

void cw_debug_command(const char *name, const char *usage, cw_debug_cmd_fn fn) {
  for (int i = 0; i < cmds.count; i++)
    if (!strcmp(cmds.data[i].name, name)) {
      cmds.data[i].fn = fn;
      cmds.data[i].usage = usage;
      return;
    }
  cmd_entry e = { name, usage, fn };
  cw_array_push(&cmds, e);
}

/* a built-in that a module may already have replaced (registration order
 * does not matter) */
static void command_default(const char *name, const char *usage, cw_debug_cmd_fn fn) {
  for (int i = 0; i < cmds.count; i++)
    if (!strcmp(cmds.data[i].name, name)) return;
  cw_debug_command(name, usage, fn);
}

static void put(cw_reply *r, const char *s, size_t n) {
  if (r->len + n + 1 > r->cap) {
    r->cap = (r->len + n + 1) * 2 + 256;
    r->buf = cw_realloc(r->buf, r->cap, CW_MEM_CORE);
  }
  memcpy(r->buf + r->len, s, n);
  r->len += n;
  r->buf[r->len] = 0;
}

void cw_reply_raw(cw_reply *r, const char *json) { put(r, json, strlen(json)); }

void cw_reply_printf(cw_reply *r, const char *fmt, ...) {
  char tmp[2048];
  va_list ap;
  va_start(ap, fmt);
  int n = vsnprintf(tmp, sizeof tmp, fmt, ap);
  va_end(ap);
  if (n >= (int)sizeof tmp) {
    char *big = cw_alloc((size_t)n + 1, CW_MEM_CORE);
    va_start(ap, fmt);
    vsnprintf(big, (size_t)n + 1, fmt, ap);
    va_end(ap);
    put(r, big, (size_t)n);
    cw_free(big);
  } else {
    put(r, tmp, (size_t)n);
  }
}

void cw_reply_str(cw_reply *r, const char *s) {
  put(r, "\"", 1);
  for (; s && *s; s++) {
    char esc[8];
    if (*s == '"' || *s == '\\') {
      esc[0] = '\\';
      esc[1] = *s;
      put(r, esc, 2);
    } else if (*s == '\n') {
      put(r, "\\n", 2);
    } else if ((unsigned char)*s < 32) {
      snprintf(esc, sizeof esc, "\\u%04x", *s);
      put(r, esc, 6);
    } else {
      put(r, s, 1);
    }
  }
  put(r, "\"", 1);
}

void cw_reply_error(cw_reply *r, const char *fmt, ...) {
  char tmp[1024];
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(tmp, sizeof tmp, fmt, ap);
  va_end(ap);
  r->len = 0;
  if (r->buf) r->buf[0] = 0;
  cw_reply_raw(r, "{\"error\":");
  cw_reply_str(r, tmp);
  cw_reply_raw(r, "}");
  r->error = true;
}

void cw_reply_defer(cw_reply *r, cw_debug_wait_fn wait, void *state) {
  r->wait = wait;
  r->wait_state = state;
}

static int split(char *s, const char **argv, int max) {
  int argc = 0;
  while (*s && argc < max) {
    while (*s == ' ' || *s == '\t' || *s == '\n') s++;
    if (!*s) break;
    if (*s == '"') {
      /* inside quotes \" and \\ escape, so a quoted string value can travel
       * as one token: set Hud text "\"a b\"" sets the text to `a b` */
      char *w = ++s;
      argv[argc++] = w;
      while (*s && *s != '"') {
        if (*s == '\\' && (s[1] == '"' || s[1] == '\\')) s++;
        *w++ = *s++;
      }
      if (*s) s++;
      *w = 0;
      continue;
    } else {
      char *w = s;
      argv[argc++] = s;
      /* a value like vec3(1, 2, 3) keeps its spaces until the closing paren;
       * a "quoted" part inside a token (outside parens) joins it as plain
       * text, as in a shell: key="a b" is the one token key=a b. Quotes
       * inside parens stay: node("/root/x") */
      int depth = 0;
      while (*s && (depth > 0 || (*s != ' ' && *s != '\t' && *s != '\n'))) {
        if (*s == '"' && depth == 0) {
          for (s++; *s && *s != '"'; s++) {
            if (*s == '\\' && (s[1] == '"' || s[1] == '\\')) s++;
            *w++ = *s;
          }
          if (*s) s++;
          continue;
        }
        if (*s == '(') depth++;
        else if (*s == ')') depth--;
        *w++ = *s++;
      }
      if (*s) s++;
      *w = 0;
    }
  }
  return argc;
}

/* ── built-in commands ──────────────────────────────────────────────── */

static void cmd_help(int argc, const char **argv, cw_reply *r) {
  cw_reply_raw(r, "{\"commands\":[");
  bool first = true;
  for (int i = 0; i < cmds.count; i++) {
    if (argc > 1 && strcmp(cmds.data[i].name, argv[1])) continue;
    if (!first) cw_reply_raw(r, ",");
    first = false;
    cw_reply_printf(r, "{\"name\":\"%s\",\"usage\":", cmds.data[i].name);
    cw_reply_str(r, cmds.data[i].usage);
    cw_reply_raw(r, "}");
  }
  cw_reply_raw(r, "]}");
}

static void cmd_stats(int argc, const char **argv, cw_reply *r) {
  (void)argc;
  (void)argv;
  cw_mem_stats m;
  cw_mem_get_stats(&m);
  cw_jobs_stats j;
  cw_jobs_get_stats(&j);
  cw_reply_printf(r, "{\"frame\":%llu,\"tick\":%llu,\"time\":%.6f,\"threads\":%d,\"jobs\":%llu,\"chunks\":%llu,\"mem\":{",
                  (unsigned long long)cw_frame_index(), (unsigned long long)cw_tick_index(), cw_time(),
                  cw_jobs_thread_count(), (unsigned long long)j.jobs, (unsigned long long)j.chunks);
  for (int i = 0; i < CW_MEM_TAG_COUNT; i++)
    cw_reply_printf(r, "%s\"%s\":%lld", i ? "," : "", cw_mem_tag_name((cw_mem_tag)i), (long long)m.bytes[i]);
  cw_reply_printf(r, "},\"mem_total\":%lld,\"mem_peak\":%lld}", (long long)m.total_bytes, (long long)m.peak_bytes);
}

/* status fields other modules add (the scene runner: tree pause, revision) */
void (*cw__debug_status_hook)(cw_reply *r);

static void status_json(cw_reply *r) {
  cw_reply_printf(r, "{\"frame\":%llu,\"tick\":%llu,\"time\":%.6f,\"paused\":%s,\"steps_left\":%u",
                  (unsigned long long)cw_frame_index(), (unsigned long long)cw_tick_index(), cw_time(),
                  cw_debug_paused() ? "true" : "false", cw_debug_steps_left());
  if (cw__debug_status_hook) cw__debug_status_hook(r);
  cw_reply_raw(r, "}");
}

static void cmd_status(int argc, const char **argv, cw_reply *r) {
  (void)argc;
  (void)argv;
  status_json(r);
}

static bool parse_flag(const char *s, bool *out) {
  if (!strcmp(s, "1") || !strcmp(s, "true") || !strcmp(s, "on")) *out = true;
  else if (!strcmp(s, "0") || !strcmp(s, "false") || !strcmp(s, "off")) *out = false;
  else return false;
  return true;
}

static void cmd_pause(int argc, const char **argv, cw_reply *r) {
  bool on = true;
  if (argc > 1 && !parse_flag(argv[1], &on)) {
    cw_reply_error(r, "pause takes 1 or 0, not '%s' (pause / pause 1 freezes the game, resume continues it)", argv[1]);
    return;
  }
  cw_debug_pause(on);
  status_json(r);
}

static void cmd_resume(int argc, const char **argv, cw_reply *r) {
  (void)argc;
  (void)argv;
  cw_debug_pause(false);
  status_json(r);
}

static bool step_wait(cw_reply *r, void *state) {
  (void)state;
  if (!r) return true; /* cancelled */
  if (cw_debug_steps_left()) return false;
  status_json(r);
  return true;
}

static void cmd_step(int argc, const char **argv, cw_reply *r) {
  long n = argc > 1 ? strtol(argv[1], NULL, 10) : 1;
  if (n < 1 || n > 100000) {
    cw_reply_error(r, "step takes a tick count from 1 to 100000, not '%s' (e.g. step 10)", argc > 1 ? argv[1] : "");
    return;
  }
  cw_debug_step((uint32_t)n);
  cw_reply_defer(r, step_wait, NULL);
}

static void cmd_quit(int argc, const char **argv, cw_reply *r) {
  (void)argc;
  (void)argv;
  cw_quit();
  cw_reply_raw(r, "{\"quitting\":true}");
}

/* For crash-isolation tests: the editor must survive its game dying. */
static void cmd_crash(int argc, const char **argv, cw_reply *r) {
  const char *how = argc > 1 ? argv[1] : "";
  if (!strcmp(how, "abort")) abort();
  if (!strcmp(how, "trap")) __builtin_trap();
#if !CW_CART
  if (!strcmp(how, "segv")) raise(SIGSEGV);
#endif
  cw_reply_error(r, "debug_crash takes abort, trap%s (it kills the game on purpose)", CW_CART ? "" : " or segv");
}

static void cmd_no_record(int argc, const char **argv, cw_reply *r) {
  (void)argc;
  (void)argv;
  cw_reply_error(r, "this game defines no input actions (cartwheel/input.h: cw_action_define), so there is no input stream to record");
}

/* ── screenshots by layer ───────────────────────────────────────────── */

static uint32_t layer_mask = CW_LAYER_ALL;
void cw_render_set_layers(uint32_t mask) { layer_mask = mask & CW_LAYER_ALL; }
uint32_t cw_render_layers(void) { return layer_mask; }

typedef struct {
  uint32_t id;
  uint32_t mask, saved_mask;
  int scale;
  bool armed, done;
  uint64_t frame;
  int w, h;
  unsigned char *png;
  int png_len;
} shot;
static shot shots[2]; /* the last two captures stay fetchable */
static uint32_t next_shot = 1;
static shot *capturing;

static shot *find_shot(uint32_t id) {
  for (int i = 0; i < 2; i++)
    if (shots[i].id == id) return &shots[i];
  return NULL;
}

static uint32_t parse_layers(const char *s) {
  uint32_t m = 0;
  char buf[64];
  snprintf(buf, sizeof buf, "%s", s);
  for (char *t = strtok(buf, "+,"); t; t = strtok(NULL, "+,")) {
    if (!strcmp(t, "world")) m |= CW_LAYER_WORLD;
    else if (!strcmp(t, "ui")) m |= CW_LAYER_UI;
    else if (!strcmp(t, "debug")) m |= CW_LAYER_DEBUG;
    else if (!strcmp(t, "all")) m |= CW_LAYER_ALL;
    else return 0;
  }
  return m;
}

typedef struct { unsigned char *data; size_t len, cap; } png_sink;
static void png_put(void *ctx, void *data, int size) {
  png_sink *k = ctx;
  if (k->len + (size_t)size > k->cap) {
    k->cap = (k->len + (size_t)size) * 2;
    k->data = realloc(k->data, k->cap);
  }
  memcpy(k->data + k->len, data, (size_t)size);
  k->len += (size_t)size;
}

/* the next frame renders only `mask`; cw__debug_frame_end reads it back */
static void capture_end(void) {
  shot *s = capturing;
  capturing = NULL;
  cw_render_set_layers(s->saved_mask);
  int w = 0, h = 0;
  cw_drawable_size(&w, &h);
  if (w <= 0 || h <= 0) {
    s->done = true;
    return;
  }
  unsigned char *px = cw_alloc((size_t)w * h * 4, CW_MEM_EDITOR);
  if (cw_gpu_backend() == CW_GPU_WEBGPU) {
    /* the frame texture (top-down), stored bottom-up as GL reads it; a cart
     * cannot wait for a readback inside its frame, so carts have none yet */
    unsigned char *top = NULL;
    int rw = 0, rh = 0;
    if (!cw__wgpu_readback(&top, &rw, &rh) || rw != w || rh != h) {
      free(top);
      cw_error("screenshot: no WebGPU readback here (%s)", CW_CART ? "WebGPU carts cannot read back inside a frame" : "readback failed");
      cw_free(px);
      s->done = true;
      return;
    }
    for (int y = 0; y < h; y++) memcpy(px + (size_t)(h - 1 - y) * w * 4, top + (size_t)y * w * 4, (size_t)w * 4);
    free(top);
  } else {
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    glPixelStorei(GL_PACK_ALIGNMENT, 1);
    glReadPixels(0, 0, w, h, GL_RGBA, GL_UNSIGNED_BYTE, px);
  }
  int k = s->scale, ow = w / k, oh = h / k;
  if (ow < 1) ow = 1;
  if (oh < 1) oh = 1;
  /* flip (GL is bottom-up) and box-filter down by `scale` */
  unsigned char *out = cw_alloc((size_t)ow * oh * 4, CW_MEM_EDITOR);
  for (int y = 0; y < oh; y++)
    for (int x = 0; x < ow; x++) {
      unsigned acc[4] = { 0 };
      for (int sy = 0; sy < k; sy++)
        for (int sx = 0; sx < k; sx++) {
          const unsigned char *p = px + ((size_t)(h - 1 - (y * k + sy)) * w + (size_t)(x * k + sx)) * 4;
          for (int c = 0; c < 4; c++) acc[c] += p[c];
        }
      for (int c = 0; c < 4; c++) out[((size_t)y * ow + x) * 4 + c] = (unsigned char)(acc[c] / (unsigned)(k * k));
    }
  cw_free(px);
  png_sink sink = { 0 };
  if (stbi_write_png_to_func(png_put, &sink, ow, oh, 4, out, ow * 4)) {
    s->png = sink.data;
    s->png_len = (int)sink.len;
  } else {
    free(sink.data);
  }
  cw_free(out);
  s->w = ow;
  s->h = oh;
  s->done = true;
}

static void shot_meta(cw_reply *r, shot *s) {
  cw_reply_printf(r, "{\"id\":%u,\"layers\":%u,\"width\":%d,\"height\":%d,\"frame\":%llu,\"png_bytes\":%d}", s->id,
                  s->mask, s->w, s->h, (unsigned long long)s->frame, s->png_len);
}

static bool shot_wait(cw_reply *r, void *state) {
  shot *s = find_shot((uint32_t)(uintptr_t)state);
  if (!r) return true;
  if (!s) {
    cw_reply_error(r, "screenshot was replaced by a newer one before it finished (take one at a time)");
    return true;
  }
  if (!s->done) return false;
  if (!s->png) cw_reply_error(r, "screenshot %u: no pixels (no window or the PNG encode failed)", s->id);
  else shot_meta(r, s);
  return true;
}

static void cmd_screenshot(int argc, const char **argv, cw_reply *r) {
  uint32_t mask = parse_layers(argc > 1 ? argv[1] : "all");
  if (!mask) {
    cw_reply_error(r, "screenshot layers are world, ui, debug or all, joined with + (e.g. screenshot world+ui 2), not '%s'", argv[1]);
    return;
  }
  int scale = argc > 2 ? atoi(argv[2]) : 1;
  if (scale < 1 || scale > 16) {
    cw_reply_error(r, "screenshot scale is 1 to 16 (a box-filtered downscale), not '%s'", argv[2]);
    return;
  }
  if (capturing) {
    cw_reply_error(r, "screenshot %u is still capturing (one at a time: wait for its reply)", capturing->id);
    return;
  }
  /* reuse the older slot */
  shot *s = shots[0].id < shots[1].id ? &shots[0] : &shots[1];
  if (s->png) free(s->png); /* stbiw allocates with malloc */
  memset(s, 0, sizeof *s);
  s->id = next_shot++;
  s->mask = mask;
  s->scale = scale;
  s->frame = cw_frame_index();
  s->saved_mask = cw_render_layers();
  cw_render_set_layers(mask);
  capturing = s;
  cw_reply_defer(r, shot_wait, (void *)(uintptr_t)s->id);
}

/* the PNG as base64 in pieces that fit the mailbox */
#define SHOT_CHUNK (96 * 1024)
static void cmd_shot_get(int argc, const char **argv, cw_reply *r) {
  if (argc < 2) {
    cw_reply_error(r, "usage: screenshot_get <id> [offset]: the id comes from a screenshot reply");
    return;
  }
  shot *s = find_shot((uint32_t)strtoul(argv[1], NULL, 10));
  if (!s || !s->done || !s->png) {
    cw_reply_error(r, "no finished screenshot %s (take one first: screenshot world; only the last two are kept)", argv[1]);
    return;
  }
  long off = argc > 2 ? strtol(argv[2], NULL, 10) : 0;
  if (off < 0 || off > s->png_len) {
    cw_reply_error(r, "offset %ld is outside screenshot %u (png_bytes %d)", off, s->id, s->png_len);
    return;
  }
  int n = s->png_len - (int)off < SHOT_CHUNK ? s->png_len - (int)off : SHOT_CHUNK;
  char *b64 = cw_base64_encode(s->png + off, (size_t)n);
  cw_reply_printf(r, "{\"id\":%u,\"offset\":%ld,\"bytes\":%d,\"png_bytes\":%d,\"more\":%s,\"base64\":\"", s->id, off, n,
                  s->png_len, off + n < s->png_len ? "true" : "false");
  cw_reply_raw(r, b64);
  cw_reply_raw(r, "\"}");
  cw_free(b64);
}

void cw__debug_frame_end(void) {
  if (capturing) capture_end();
}

/* ── running requests ───────────────────────────────────────────────── */

void cw__vfs_register_commands(void); /* core/vfs.c */
void cw__debug_cmd_fields(int argc, const char **argv, cw_reply *r); /* debug/fields.c */

static void ensure_builtins(void) {
  static bool builtin;
  if (builtin) return;
  builtin = true;
  command_default("help", "help [command]: list commands (or one) with usage", cmd_help);
  command_default("stats", "stats: frame, tick, memory per tag, job counts", cmd_stats);
  command_default("status", "status: frame, tick, the debugger's pause and steps left (and the tree's state in scene apps)", cmd_status);
  command_default("pause", "pause [1|0]: the debugger's pause: freeze the whole simulation (frames still render, "
                           "the channel still answers); pause 0 = resume. The game's own pause is tree_pause", cmd_pause);
  command_default("resume", "resume: end the debugger's pause", cmd_resume);
  command_default("step", "step [ticks]: pause and run exactly that many fixed ticks (default 1), one per frame; "
                          "replies when they ran, with the new tick", cmd_step);
  command_default("screenshot", "screenshot [world|ui|debug|all (join with +)] [scale]: render the next frame with only "
                                "those layers and capture it as PNG; replies {id,width,height,png_bytes} when done. "
                                "Fetch with screenshot_get", cmd_screenshot);
  command_default("screenshot_get", "screenshot_get <id> [offset]: a screenshot's PNG as base64, 96 KiB per reply "
                                    "(repeat with offset += bytes while more is true)", cmd_shot_get);
  command_default("record", "record start|stop|status|fetch|replay: the input stream (games with input actions)", cmd_no_record);
  command_default("fields", "fields [name...]: debug fields' values (cw_debug_field), all of them when none is named",
                  cw__debug_cmd_fields);
  command_default("quit", "quit: end the game cleanly", cmd_quit);
  command_default("debug_crash", "debug_crash abort|trap|segv: kill the game on purpose (crash-isolation tests)", cmd_crash);
  cw__vfs_register_commands();
}

/* Before the app's init a request for a command not registered yet is held
 * (in order: requests behind it wait too) until cw__debug_channel_app_ready. */
static bool must_wait(const char *request) {
  if (app_ready) return false;
  ensure_builtins();
  const char *s = request;
  while (*s == ' ' || *s == '\t' || *s == '\n') s++;
  size_t n = strcspn(s, " \t\n");
  if (!n) return false; /* empty: answered now (an error) */
  for (int i = 0; i < cmds.count; i++)
    if (strlen(cmds.data[i].name) == n && !strncmp(cmds.data[i].name, s, n)) return false;
  return true;
}

/* Runs a request into r. Returns false when the command deferred its reply. */
static bool run_into(const char *request, cw_reply *r) {
  ensure_builtins();
  size_t n = strlen(request);
  char *copy = cw_alloc(n + 1, CW_MEM_CORE);
  memcpy(copy, request, n + 1);
  const char *argv[32];
  int argc = split(copy, argv, 32);
  if (!argc) {
    cw_reply_error(r, "empty request (try help)");
  } else {
    bool found = false;
    for (int i = 0; i < cmds.count && !found; i++)
      if (!strcmp(cmds.data[i].name, argv[0])) {
        found = true;
        cmds.data[i].fn(argc, argv, r);
      }
    if (!found) cw_reply_error(r, "unknown command '%s' (help lists this game's commands)", argv[0]);
  }
  cw_free(copy);
  if (r->wait && !r->error) {
    r->len = 0;
    if (r->buf) r->buf[0] = 0;
    return false;
  }
  if (!r->buf) cw_reply_raw(r, "{}");
  return true;
}

char *cw_debug_run(const char *request) {
  cw_reply r = { 0 };
  if (!run_into(request, &r)) {
    r.wait(NULL, r.wait_state); /* nothing here can wait for frames */
    cw_reply_raw(&r, "{\"deferred\":true}");
  }
  return r.buf;
}

/* requests waiting for frames: one per mailbox, any number per socket */
typedef struct {
  int transport; /* 0 mailbox, 1 socket */
  uint32_t id;
  cw_debug_wait_fn wait;
  void *state;
} pending;
static CW_ARRAY(pending) pendings;
static void remove_pending(int i) {
  memmove(pendings.data + i, pendings.data + i + 1, sizeof *pendings.data * (size_t)(pendings.count - i - 1));
  pendings.count--;
}

static void mailbox_deliver(const char *reply) {
  size_t n = strlen(reply);
  if (n >= REPLY_CAP) {
    n = (size_t)snprintf(reply_buf, REPLY_CAP,
                         "{\"error\":\"reply is %zu bytes, over the %d KiB mailbox: ask for less (tree <node> <depth>, "
                         "props <node>, get <node> <prop...>) or use a socket transport (cartwheel/remote.h)\"}",
                         n, REPLY_CAP / 1024);
  } else {
    memcpy(reply_buf, reply, n);
    reply_buf[n] = 0;
  }
  reply_len = (uint32_t)n;
  reply_seq = handled_seq;
}

/* ── the socket transport (native) ──────────────────────────────────── */

#if !CW_CART
#if defined(_WIN32)
/* Winsock: a SOCKET handle, closesocket, no SIGPIPE to suppress; the socket
 * stays blocking and the poll asks select() whether a recv would block */
typedef SOCKET sock_t;
#define SOCK_NONE INVALID_SOCKET
#define sock_valid(s) ((s) != INVALID_SOCKET)
#define sock_closefd closesocket
#define MSG_NOSIGNAL 0
#define sock_errstr() "winsock error"
static int sock_errno(void) { return WSAGetLastError() == WSAEWOULDBLOCK ? EAGAIN : EIO; }
#else
typedef int sock_t;
#define SOCK_NONE (-1)
#define sock_valid(s) ((s) >= 0)
#define sock_closefd close
#define sock_errstr() strerror(errno)
static int sock_errno(void) { return errno; }
#endif
static sock_t sock = SOCK_NONE;
static CW_ARRAY(char) inbox;

bool cw__debug_remote_requested(void) {
  const char *e = getenv("CW_REMOTE");
  return e && *e;
}

static void put_u32(unsigned char *p, uint32_t v) {
  p[0] = (unsigned char)v;
  p[1] = (unsigned char)(v >> 8);
  p[2] = (unsigned char)(v >> 16);
  p[3] = (unsigned char)(v >> 24);
}
static uint32_t get_u32(const unsigned char *p) {
  return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}

static void sock_close(void) {
  if (sock_valid(sock)) sock_closefd(sock);
  sock = SOCK_NONE;
}

static void sock_send(uint32_t id, const char *payload, size_t len) {
  if (!sock_valid(sock)) return;
  unsigned char hdr[8];
  put_u32(hdr, (uint32_t)len);
  put_u32(hdr + 4, id);
  const char *parts[2] = { (const char *)hdr, payload };
  size_t sizes[2] = { 8, len };
  for (int k = 0; k < 2; k++) {
    size_t off = 0;
    while (off < sizes[k]) {
      long w = (long)send(sock, parts[k] + off, (int)(sizes[k] - off), MSG_NOSIGNAL);
      if (w < 0 && sock_errno() == EINTR) continue;
      if (w <= 0) {
        cw_warn("debug channel: the editor's socket broke (%s); quitting", sock_errstr());
        sock_close();
        cw_quit();
        return;
      }
      off += (size_t)w;
    }
  }
}

static void sock_open(void) {
  const char *e = getenv("CW_REMOTE");
  if (!e || !*e) return;
#if defined(_WIN32)
  WSADATA wsa;
  if (WSAStartup(MAKEWORD(2, 2), &wsa)) {
    cw_error("CW_REMOTE=%s: WSAStartup failed", e);
    return;
  }
#endif
  if (!strncmp(e, "fd:", 3)) {
    sock = (sock_t)atoi(e + 3);
  } else if (!strncmp(e, "tcp:", 4)) {
    char host[256];
    snprintf(host, sizeof host, "%s", e + 4);
    char *colon = strrchr(host, ':');
    if (!colon) {
      cw_error("CW_REMOTE=%s: expected tcp:HOST:PORT (e.g. tcp:127.0.0.1:7400)", e);
      return;
    }
    *colon = 0;
    struct addrinfo hints = { .ai_family = AF_UNSPEC, .ai_socktype = SOCK_STREAM }, *ai = NULL;
    if (getaddrinfo(host, colon + 1, &hints, &ai) || !ai) {
      cw_error("CW_REMOTE=%s: cannot resolve %s", e, host);
      return;
    }
    sock = socket(ai->ai_family, SOCK_STREAM, 0);
    if (sock_valid(sock) && connect(sock, ai->ai_addr, (int)ai->ai_addrlen)) {
      cw_error("CW_REMOTE=%s: connect failed (%s): is the editor listening?", e, sock_errstr());
      sock_close();
    }
    freeaddrinfo(ai);
  } else {
    cw_error("CW_REMOTE=%s: expected fd:N or tcp:HOST:PORT", e);
    return;
  }
  if (!sock_valid(sock)) return;
#if !defined(_WIN32)
  fcntl(sock, F_SETFD, FD_CLOEXEC);
#endif
  char hello[512];
  int n = snprintf(hello, sizeof hello, "{\"event\":\"hello\",\"protocol\":1,\"pid\":%d,\"frame\":%llu,\"tick\":%llu}",
                   (int)getpid(), (unsigned long long)cw_frame_index(), (unsigned long long)cw_tick_index());
  sock_send(0, hello, (size_t)n);
}

static void sock_request(uint32_t id, const char *text) {
  cw_reply r = { 0 };
  if (run_into(text, &r)) {
    sock_send(id, r.buf, r.len);
  } else {
    pending p = { 1, id, r.wait, r.wait_state };
    cw_array_push(&pendings, p);
  }
  cw_free(r.buf);
}

static void sock_poll(void) {
  if (!sock_valid(sock)) return;
  for (;;) {
    char buf[16384];
#if defined(_WIN32)
    fd_set rd;
    FD_ZERO(&rd);
    FD_SET(sock, &rd);
    struct timeval tv = { 0, 0 };
    if (select(0, &rd, NULL, NULL, &tv) == 0) break; /* nothing to read: a recv would block */
    long n = (long)recv(sock, buf, (int)sizeof buf, 0);
#else
    long n = (long)recv(sock, buf, sizeof buf, MSG_DONTWAIT);
#endif
    if (n > 0) {
      for (long i = 0; i < n; i++) cw_array_push(&inbox, buf[i]);
      continue;
    }
    if (n < 0 && sock_errno() == EINTR) continue;
    if (n < 0 && (sock_errno() == EAGAIN || sock_errno() == EWOULDBLOCK)) break;
    /* EOF or an error: the editor is gone */
    cw_log(CW_LOG_INFO, "debug channel: the editor closed the connection; quitting");
    sock_close();
    cw_quit();
    return;
  }
  size_t off = 0;
  while ((size_t)inbox.count - off >= 8) {
    const unsigned char *h = (const unsigned char *)inbox.data + off;
    uint32_t len = get_u32(h), id = get_u32(h + 4);
    if ((size_t)inbox.count - off - 8 < len) break;
    if (!app_ready) { /* held until the app's init when its command is not registered yet */
      char *peek = cw_alloc((size_t)len + 1, CW_MEM_CORE);
      memcpy(peek, inbox.data + off + 8, len);
      peek[len] = 0;
      bool wait = must_wait(peek);
      cw_free(peek);
      if (wait) break;
    }
    char *text = cw_alloc((size_t)len + 1, CW_MEM_CORE);
    memcpy(text, inbox.data + off + 8, len);
    text[len] = 0;
    off += 8 + (size_t)len;
    sock_request(id, text);
    cw_free(text);
  }
  if (off) {
    memmove(inbox.data, inbox.data + off, (size_t)inbox.count - off);
    inbox.count -= (int)off;
  }
}
#else
bool cw__debug_remote_requested(void) { return false; }
static void sock_open(void) {}
static void sock_poll(void) {}
static void sock_send(uint32_t id, const char *payload, size_t len) {
  (void)id;
  (void)payload;
  (void)len;
}
#endif

void cw__debug_channel_init(void) {
  if (inited) return;
  inited = true;
  cw_debug_bytes("dbg.cmd", cmd_buf, CMD_CAP);
  cw_debug_u32("dbg.cmd_seq", &cmd_seq);
  cw_debug_bytes("dbg.reply", reply_buf, REPLY_CAP);
  cw_debug_u32("dbg.reply_seq", &reply_seq);
  cw_debug_u32("dbg.reply_len", &reply_len);
  sock_open();
}

void cw__debug_channel_poll(void) {
  if (!inited) return;
  /* deferred replies whose frames have run */
  for (int i = 0; i < pendings.count; i++) {
    pending p = pendings.data[i];
    cw_reply r = { 0 };
    if (!p.wait(&r, p.state)) {
      cw_free(r.buf);
      continue;
    }
    if (!r.buf) cw_reply_raw(&r, "{}");
    if (p.transport == 0) mailbox_deliver(r.buf);
    else sock_send(p.id, r.buf, r.len);
    cw_free(r.buf);
    remove_pending(i);
    i--;
  }
  cmd_buf[CMD_CAP - 1] = 0;
  if (cmd_seq != handled_seq && !must_wait(cmd_buf)) {
    /* a new mailbox request replaces one still waiting */
    for (int i = 0; i < pendings.count; i++)
      if (pendings.data[i].transport == 0) {
        pendings.data[i].wait(NULL, pendings.data[i].state);
        remove_pending(i);
        i--;
      }
    handled_seq = cmd_seq;
    cw_reply r = { 0 };
    if (run_into(cmd_buf, &r)) {
      mailbox_deliver(r.buf);
    } else {
      pending p = { 0, handled_seq, r.wait, r.wait_state };
      cw_array_push(&pendings, p);
    }
    cw_free(r.buf);
  }
  sock_poll();
}

void cw__debug_channel_app_ready(void) {
  if (app_ready) return;
  app_ready = true;
  cw__debug_channel_poll(); /* the requests held for the app's commands */
}

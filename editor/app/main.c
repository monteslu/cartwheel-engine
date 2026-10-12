/* The cartwheel editor app: natively `editor [project dir]` (CW_PROJECT
 * works too), as a cart editor.wasc (its bundled demo project plus the
 * in-memory project layer). Built with EDITOR_SESSION it plays a scripted
 * editing session (editor/tests/session.inc) for the cross-host gate. */
#include "cartwheel/gl.h"
#include "../core/internal.h"

/* An editor built for a game (games/<game>/editor_types.c) registers the
 * game's node types first, so its scenes open: -DCWED_APP_TYPES=<fn>. */
#ifdef CWED_APP_TYPES
void CWED_APP_TYPES(void);
#endif
/* A session app may play another script: -DEDITOR_SESSION_INC="file". */
#ifndef EDITOR_SESSION_INC
#define EDITOR_SESSION_INC "../tests/session.inc"
#endif

static int g_argc;
static char **g_argv;

#ifdef EDITOR_SESSION
typedef struct { int frame; const char *line; } step;
static const step SESSION[] = {
#include EDITOR_SESSION_INC
};
static int session_next;
static int32_t session_done, session_failed;
static uint32_t session_hash = CW_HASH32_INIT;

static void session_step(void) {
  int f = (int)cw_frame_index();
  while (session_next < (int)CW_ARRAY_LEN(SESSION) && SESSION[session_next].frame <= f) {
    char *json = NULL;
    bool ok = cwed_run(SESSION[session_next].line, &json);
    if (!ok) session_failed++;
    if (json) session_hash = cw_hash32(json, strlen(json), session_hash), cw_free(json);
    session_next++;
  }
  if (session_next == (int)CW_ARRAY_LEN(SESSION)) session_done = 1;
}
#endif

static void init(void *u) {
  (void)u;
#ifdef CWED_APP_TYPES
  cw_scene_register_builtin_types();
  CWED_APP_TYPES();
#endif
  cwed_init(g_argc, g_argv);
  cwed_debug_fields();
  cw_ui_debug_register("ui");
#if !CW_CART
  /* CW_EDITOR_RUN="line;line": command lines run at startup (natively), the
   * quick way to open a scene and set a view for a screenshot */
  const char *run = getenv("CW_EDITOR_RUN");
  if (run) {
    char *all = cw_strdup(run, CW_MEM_EDITOR), *save = NULL;
    for (char *c = strtok_r(all, ";", &save); c; c = strtok_r(NULL, ";", &save)) {
      char *json = NULL;
      bool ok = cwed_run(c, &json);
      if (!ok) cw_warn("CW_EDITOR_RUN: %s: %s", c, json ? json : "");
      cw_free(json);
    }
    cw_free(all);
  }
#endif
#ifdef EDITOR_SESSION
  cw_debug_i32("session.done", &session_done);
  cw_debug_i32("session.failed", &session_failed);
  cw_debug_u32("session.reply_hash", &session_hash);
#endif
}

void cwed__fields_frame(void);

static void update(void *u, double dt) {
  (void)u;
#ifdef EDITOR_SESSION
  session_step();
#endif
  cwed_frame(dt);
  cwed__fields_frame();
}

static void render(void *u, double a) {
  (void)u, (void)a;
  int w, h;
  cw_drawable_size(&w, &h);
  glViewport(0, 0, w, h);
  glClearColor(0.1f, 0.11f, 0.13f, 1);
  glClear(GL_COLOR_BUFFER_BIT);
  cw_ui_render(cwed_ui_tree());
}

static void shutdown(void *u) {
  (void)u;
  cwed_shutdown();
}

cw_app_desc cw_main(int argc, char **argv) {
  g_argc = argc;
  g_argv = argv;
  return (cw_app_desc){ .title = "cartwheel editor", .width = 1280, .height = 720, .render_scale = 1, .init = init, .update = update,
                        .render = render, .shutdown = shutdown, .debug = true };
}

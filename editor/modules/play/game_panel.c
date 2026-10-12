/* Game panel (M4.3): the play session at a glance and the game's frames.
 *
 * Where the game's picture lives: a NATIVE game runs in its own window (its
 * own process and GL context: full frame rate, its own input, nothing copied
 * through the editor, and a crash takes only that window). This panel then
 * shows layer captures on demand. A ROMDEV game (and a headless native one)
 * has no window here, so the panel shows its frames live: `screenshot all`
 * every few frames, fetched with screenshot_get. play.preview overrides.
 *
 * Also: the status line, a crash/exit report banner with Relaunch, the layer
 * capture (world / ui / debug), and in the cart editor the reason Play is
 * not available. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cartwheel/render2d.h"
#include "cartwheel/vfs.h"
#include "play.h"

static cw_node status, banner, image, caption, layers, live, b_main, b_relaunch, b_capture, b_crash;
static cw_texture tex;
static int gen_seen = -1, frames;
static bool live_on, live_set_by_user, syncing, banner_shown;

static const char *const LAYERS[] = { "all", "world", "ui", "debug", "world+ui", NULL };

/* ── showing a PNG ────────────────────────────────────────────────────── */

void pl_game_show_png(const void *png, size_t len, const char *what) {
  if (!cw_node_valid(image)) return;
  cw_texture t = cw_texture_from_memory(png, len, &(cw_texture_desc){ .standalone = true });
  if (!cw_texture_valid(t)) return;
  cw_texture_rect_set_texture(image, t);
  if (cw_texture_valid(tex)) cw_texture_destroy(tex);
  tex = t;
  int w = 0, h = 0;
  cw_texture_size(t, &w, &h);
  cw_label_set_text(caption, cw_frame_printf("%s  ·  %dx%d", what, w, h));
}

/* ── live preview: screenshot, then screenshot_get pieces ─────────────── */

static struct {
  int state; /* 0 idle, 1 capturing, 2 fetching */
  unsigned id;
  size_t total, have, cap;
  unsigned char *buf;
  int scale;
  long long tick;
  uint32_t req;
} P = { .scale = 1 };

static void preview_reset(void) {
  free(P.buf);
  P.buf = NULL;
  P.have = P.cap = P.total = 0;
  P.state = 0;
}

static void on_part(const cw_remote_reply *r, void *u);
static void fetch_next(void) {
  P.state = 2;
  uint32_t id = pl_send(cw_frame_printf("screenshot_get %u %zu", P.id, P.have), on_part, NULL);
  if (!id) preview_reset();
  else if (P.state) P.req = id;
}

static void on_part(const cw_remote_reply *r, void *u) {
  (void)u;
  pj *v = r->ok ? pj_parse(r->json) : NULL;
  const char *b64 = pj_str(v, "base64");
  size_t n = 0;
  void *bytes = b64 ? cw_base64_decode(b64, &n) : NULL;
  pj_free(v);
  if (!bytes || !n) {
    cw_free(bytes);
    preview_reset();
    return;
  }
  if (P.have + n > P.cap) {
    P.cap = P.have + n + 4096;
    P.buf = realloc(P.buf, P.cap);
  }
  memcpy(P.buf + P.have, bytes, n);
  P.have += n;
  cw_free(bytes);
  if (P.have < P.total) {
    fetch_next();
    return;
  }
  pl_game_show_png(P.buf, P.have, cw_frame_printf("live, tick %lld", P.tick));
  int w = 0, h = 0;
  if (cw_texture_valid(tex)) cw_texture_size(tex, &w, &h);
  if (w * P.scale > 900 && P.scale < 4) P.scale++; /* keep the preview cheap: about 640 px wide */
  preview_reset();
}

static void on_shot(const cw_remote_reply *r, void *u) {
  (void)u;
  pj *v = r->ok ? pj_parse(r->json) : NULL;
  P.id = (unsigned)pj_num(v, "id", 0);
  P.total = (size_t)pj_num(v, "png_bytes", 0);
  pj_free(v);
  if (!P.id || !P.total) {
    preview_reset();
    return;
  }
  P.tick = pl_tick();
  P.have = 0;
  fetch_next();
}

static void preview_tick(void) {
  if (!live_on || !pl_running() || P.state || !cwed_panel_visible("game")) return;
  if (frames % (pl_poll_frames() * 2) != 0) return;
  P.state = 1;
  const char *l = LAYERS[cw_option_button_selected(layers) < 0 ? 0 : cw_option_button_selected(layers)];
  uint32_t id = pl_send(cw_frame_printf("screenshot %s %d", l, P.scale), on_shot, NULL);
  if (!id) preview_reset();
  else if (P.state == 1) P.req = id;
}

void pl_game_preview_drain(void) {
  for (int i = 0; i < 64 && P.state; i++)
    if (!pl_wait(P.req, 10000)) {
      preview_reset();
      break;
    }
}

/* ── controls ─────────────────────────────────────────────────────────── */

static void on_button(cw_node b, const cw_value *a, int argc, void *user) {
  (void)b, (void)a, (void)argc;
  const char *what = user;
  if (!strcmp(what, "main")) cwed_run("play.main", NULL);
  else if (!strcmp(what, "relaunch")) cwed_run("play.relaunch", NULL);
  else if (!strcmp(what, "crash")) cwed_run("play.crash", NULL);
  else if (!strcmp(what, "capture")) {
    int s = cw_option_button_selected(layers);
    cwed_runf("play.screenshot layers=%s", LAYERS[s < 0 ? 0 : s]);
  }
}

static void on_live(cw_node b, const cw_value *a, int argc, void *user) {
  (void)b, (void)argc, (void)user;
  if (syncing) return;
  live_on = a[0].b;
  live_set_by_user = true;
}

static cw_node button(const char *name, const char *text, const char *tip, const char *what) {
  cw_node b = cw_node_new("Button", name);
  cw_button_set_text(b, text);
  cw_control_set_tooltip(b, tip);
  cw_node_set(b, "focus_mode", cw_val_int(CW_FOCUS_NONE));
  cw_connect(b, "pressed", (cw_node){ 0 }, NULL, on_button, (void *)what, 0);
  return b;
}

static cw_node create(void *user) {
  (void)user;
  cw_node box = cw_node_new("VBoxContainer", NULL);
  cw_node bar = cw_node_new("HBoxContainer", "Bar");
  status = cw_node_new("Label", "Status");
  cw_control_set_size_flags(status, CW_SIZE_EXPAND_FILL, CW_SIZE_SHRINK_CENTER);
  cw_node_set(status, "ellipsis", cw_val_bool(true));
  cw_node_add_child(bar, status);
  cw_node_add_child(bar, b_main = button("PlayMain", "Play Main", "Run the project's main scene (play.main)", "main"));
  cw_node_add_child(bar, b_relaunch = button("Relaunch", "Relaunch", "Start the game again (play.relaunch)", "relaunch"));
  layers = cw_node_new("OptionButton", "Layers");
  for (int i = 0; LAYERS[i]; i++) cw_option_button_add_item(layers, LAYERS[i]);
  cw_control_set_tooltip(layers, "Which layers a capture (and the live preview) draws");
  cw_node_add_child(bar, layers);
  cw_node_add_child(bar, b_capture = button("Capture", "Capture", "Capture the game's next frame with these layers (play.screenshot)", "capture"));
  live = cw_node_new("CheckBox", "Live");
  cw_button_set_text(live, "Live");
  cw_control_set_tooltip(live, "Show the game's frames here as it runs");
  cw_node_set(live, "focus_mode", cw_val_int(CW_FOCUS_NONE));
  cw_connect(live, "toggled", (cw_node){ 0 }, NULL, on_live, NULL, 0);
  cw_node_add_child(bar, live);
  cw_node_add_child(bar, b_crash = button("Crash", "Crash", "Crash the game on purpose to test the crash report (play.crash)", "crash"));
  cw_node_add_child(box, bar);
  banner = cw_node_new("Label", "Banner");
  cw_node_set(banner, "autowrap", cw_val_bool(true));
  cw_node_set(banner, "theme_overrides", cw_val_string("font_color = #ff8a7a; font_size = 13"));
  cw_control_set_visible(banner, false);
  cw_node_add_child(box, banner);
  image = cw_node_new("TextureRect", "Frame");
  cw_node_set(image, "stretch_mode", cw_val_int(5)); /* keep aspect, centered */
  cw_control_set_size_flags(image, CW_SIZE_EXPAND_FILL, CW_SIZE_EXPAND_FILL);
  cw_node_add_child(box, image);
  caption = cw_node_new("Label", "Caption");
  cw_node_set(caption, "theme_overrides", cw_val_string("font_size = 12; font_color = #8a93a3"));
  cw_node_add_child(box, caption);
  return box;
}

static void refresh(void) {
  cw_label_set_text(status, pl_status());
  const char *msg = !pl_available() ? pl_unavailable_why() : pl_crash();
  if (msg) cw_label_set_text(banner, msg);
  cw_control_set_visible(banner, msg != NULL);
  banner_shown = msg != NULL;
  cw_node_set(b_main, "disabled", cw_val_bool(!pl_available()));
  cw_node_set(b_relaunch, "disabled", cw_val_bool(!pl_available() || !*pl_scene()));
  cw_node_set(b_capture, "disabled", cw_val_bool(!pl_running()));
  cw_node_set(b_crash, "disabled", cw_val_bool(!pl_running()));
  cw_node_set(layers, "disabled", cw_val_bool(!pl_available()));
  cw_node_set(live, "disabled", cw_val_bool(!pl_available()));
  if (!pl_active() && !cw_texture_valid(tex))
    cw_label_set_text(caption, !pl_available() ? "" : "Native games run in their own window; romdev and headless games show here.");
}

void pl_game_frame(void) {
  if (!cw_node_valid(status)) return;
  frames++;
  if (gen_seen != pl_generation()) {
    gen_seen = pl_generation();
    preview_reset();
    if (!live_set_by_user) {
      live_on = pl_preview_default();
      syncing = true;
      cw_button_set_pressed(live, live_on);
      syncing = false;
    }
    if (pl_active() && live_on) cwed_run("panel.show id=game", NULL); /* the only place this game is visible */
  }
  refresh();
  preview_tick();
}

void pl_game_widgets(cwed_json *j) {
  cwed_json_key(j, "game");
  cwed_json_raw(j, "{");
  pl_rect_json(j, "play_main", cw_control_global_rect(b_main));
  pl_rect_json(j, "relaunch", cw_control_global_rect(b_relaunch));
  pl_rect_json(j, "layers", cw_control_global_rect(layers));
  pl_rect_json(j, "capture", cw_control_global_rect(b_capture));
  pl_rect_json(j, "live", cw_control_global_rect(live));
  pl_rect_json(j, "crash", cw_control_global_rect(b_crash));
  pl_rect_json(j, "frame", cw_control_global_rect(image));
  cwed_json_key(j, "banner");
  cwed_json_str(j, banner_shown ? cw_label_text(banner) : "");
  cwed_json_key(j, "status");
  cwed_json_str(j, cw_label_text(status));
  cwed_json_key(j, "caption");
  cwed_json_str(j, cw_label_text(caption));
  cwed_json_printf(j, ",\"live\":%s,\"has_frame\":%s", live_on ? "true" : "false", cw_texture_valid(tex) ? "true" : "false");
  cwed_json_raw(j, "}");
}

void pl_game_init(void) {
  cwed_panel_register(&(cwed_panel_desc){ .id = "game", .title = "Game", .slot = "center", .order = 80, .create = create });
}

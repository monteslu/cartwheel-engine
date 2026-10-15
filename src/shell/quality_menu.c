/* The graphics settings page (cartwheel/quality_menu.h). */
#include <stdio.h>
#include <string.h>

#include "cartwheel/quality_menu.h"
#include "cartwheel/core.h"
#include "cartwheel/debug.h"
#include "cartwheel/platform.h"
#include "cartwheel/quality.h"
#include "cartwheel/ui.h"

enum { MAX_ROWS = 2 + CW_GFX_ENGINE_COUNT + CW_GFX_MAX_LEVERS };

static struct {
  bool open, changed;
  int row;
  uint64_t last_frame;
  float prev_lx[CW_MAX_PADS], prev_ly[CW_MAX_PADS];
  cw_rect rects[MAX_ROWS];
  int n_rects;
} M = { .last_frame = UINT64_MAX };

/* rows: 0 the preset, 1..count the settings, then Back */
static int rows(void) { return 2 + cw_gfx_count(); }
static int back_row(void) { return rows() - 1; }

/* debug channel: "quality_menu [1|0]" opens or closes the page (a test's screenshot of it) */
static void cmd_menu(int argc, const char **argv, cw_reply *r) {
  if (argc > 1) {
    if (argv[1][0] == '1') cw_quality_menu_open();
    else cw_quality_menu_close();
  }
  cw_reply_printf(r, "{\"open\":%s,\"row\":%d}", M.open ? "true" : "false", M.row);
}
void cw__quality_menu_register(void); /* quality.c, at startup */
void cw__quality_menu_register(void) {
  static bool done;
  if (done) return;
  done = true;
  cw_debug_command("quality_menu", "quality_menu [1|0]: the graphics settings page (cartwheel/quality_menu.h)", cmd_menu);
}

void cw_quality_menu_open(void) {
  M.open = true;
  M.changed = false;
  M.row = 0;
  M.n_rects = 0;
}
void cw_quality_menu_close(void) { M.open = false; }
bool cw_quality_menu_is_open(void) { return M.open; }
bool cw_quality_menu_changed(void) { return M.changed; }

static void change(int row, int dir) {
  if (row == 0) {
    if (dir < 0) cw_quality_cycle_back();
    else cw_quality_cycle();
  } else if (row < back_row()) cw_gfx_step(row - 1, dir);
  else return;
  M.changed = true;
}

void cw_quality_menu_input(int dy, int dx, bool accept, bool back) {
  if (!M.open) return;
  int n = rows();
  if (dy) M.row = ((M.row + dy) % n + n) % n;
  if (dx) change(M.row, dx);
  if (accept) {
    if (M.row == back_row()) M.open = false;
    else change(M.row, 1);
  }
  if (back) M.open = false;
}

bool cw_quality_menu_tap(cw_vec2 p) {
  if (!M.open) return false;
  for (int i = 0; i < M.n_rects; i++) {
    cw_rect r = M.rects[i];
    if (p.x >= r.x && p.x < r.x + r.w && p.y >= r.y && p.y < r.y + r.h) {
      M.row = i;
      cw_quality_menu_input(0, 0, true, false);
      return true;
    }
  }
  return false;
}

bool cw_quality_menu_update(void) {
  if (!M.open) return false;
  if (cw_frame_index() == M.last_frame) return true;
  M.last_frame = cw_frame_index();
  const cw_raw_input *in = cw_input_raw();
  if (!in) return true;
  const uint8_t *k = in->keys_pressed;
  /* SDL scancodes: arrows, W A S D, Return, Space, Escape, Backspace */
  int dy = (k[81] || k[22]) - (k[82] || k[26]);
  int dx = (k[79] || k[7]) - (k[80] || k[4]);
  bool accept = k[40] || k[44], back = k[41] || k[42];
  for (int p = 0; p < CW_MAX_PADS; p++) {
    const cw_pad_state *pd = &in->pads[p];
    if (!pd->connected) {
      M.prev_lx[p] = M.prev_ly[p] = 0;
      continue;
    }
    uint32_t b = pd->pressed;
    if (b & (1u << CW_PAD_UP)) dy = -1;
    if (b & (1u << CW_PAD_DOWN)) dy = 1;
    if (b & (1u << CW_PAD_LEFT)) dx = -1;
    if (b & (1u << CW_PAD_RIGHT)) dx = 1;
    if (b & (1u << CW_PAD_SOUTH)) accept = true;
    if (b & (1u << CW_PAD_EAST)) back = true;
    /* the left stick: past half way, once per push */
    float lx = pd->axes[CW_AXIS_LX], ly = pd->axes[CW_AXIS_LY];
    if (ly < -0.5f && M.prev_ly[p] >= -0.5f) dy = -1;
    if (ly > 0.5f && M.prev_ly[p] <= 0.5f) dy = 1;
    if (lx < -0.5f && M.prev_lx[p] >= -0.5f) dx = -1;
    if (lx > 0.5f && M.prev_lx[p] <= 0.5f) dx = 1;
    M.prev_lx[p] = lx;
    M.prev_ly[p] = ly;
  }
  if ((in->mouse_pressed & 1) && cw_quality_menu_tap(cw_r2d_window_to_canvas(cw_v2(in->mouse_x, in->mouse_y)))) return true;
  cw_quality_menu_input(dy, dx, accept, back);
  return true;
}

static void text(cw_font f, const char *s, float x, float y, float size, cw_color c, int align /* 0 left 1 center 2 right */) {
  cw_text_style st = { .size = size, .color = c, .outline_width = size * 0.06f, .outline_color = cw_rgba(0, 0, 0, 0.8f) };
  if (align) {
    cw_text_metrics m = cw_font_measure(f, s, &st);
    x -= align == 1 ? m.width * 0.5f : m.width;
  }
  cw_r2d_text(f, s, cw_v2(x, y), &st);
}

void cw_quality_menu_draw(const cw_quality_menu_style *style) {
  if (!M.open) return;
  cw_quality_menu_style st = style ? *style : (cw_quality_menu_style){ 0 };
  if (!st.font.id) st.font = cw_ui_font_sans();
  if (st.accent.a == 0) st.accent = cw_rgba(1.0f, 0.78f, 0.25f, 1);
  if (!st.set_backdrop) st.backdrop = cw_rgba(0.02f, 0.02f, 0.05f, 0.92f);
  if (!st.title) st.title = "Graphics";
  cw_vec2 sz = cw_r2d_canvas_size();
  float u = st.size > 0 ? st.size : sz.y / 24.0f;
  if (st.backdrop.a > 0) cw_r2d_rect(cw_rect_make(0, 0, sz.x, sz.y), st.backdrop);
  float cx = sz.x * 0.5f;
  text(st.font, st.title, cx, sz.y * 0.08f, u * 1.6f, cw_rgba(1, 1, 1, 1), 1);
  /* two columns about the centre: names right-aligned left of it, values left-aligned right of it */
  int n = rows();
  float y = sz.y * 0.08f + u * 2.6f, step = u * 1.45f;
  float gap = u * 0.8f;
  M.n_rects = n < MAX_ROWS ? n : MAX_ROWS;
  for (int i = 0; i < n && i < MAX_ROWS; i++) {
    bool sel = i == M.row;
    cw_color c = sel ? st.accent : cw_rgba(1, 1, 1, 0.88f);
    if (sel) cw_r2d_rect(cw_rect_make(cx - u * 15, y - u * 0.2f, u * 30, step * 0.92f), cw_rgba(1, 1, 1, 0.08f));
    if (i == back_row()) {
      y += u * 0.3f;
      text(st.font, "Back", cx, y, u, c, 1);
    } else {
      const char *name = i == 0 ? "Preset" : cw_gfx_name(i - 1);
      char val[64];
      if (i == 0) snprintf(val, sizeof val, "<  %s  >", cw_quality_label());
      else snprintf(val, sizeof val, "<  %s  >", cw_gfx_value_label(i - 1));
      text(st.font, name, cx - gap, y, u, c, 2);
      text(st.font, val, cx + gap, y, u, c, 0);
    }
    M.rects[i] = cw_rect_make(cx - u * 15, y - u * 0.2f, u * 30, step * 0.92f);
    y += step;
    if (i == 0) y += u * 0.35f; /* the preset apart from the settings it sets */
  }
  const char *hint = cw_quality_is_auto() ? "Auto keeps the frame under budget by changing these as you play"
                                          : "up / down to choose, left / right to change";
  text(st.font, hint, cx, sz.y - u * 1.8f, u * 0.7f, cw_rgba(1, 1, 1, 0.6f), 1);
}

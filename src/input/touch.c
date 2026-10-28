/* Touch controls: the rect controls a game lays out itself, and the
 * anchored ones (thumbsticks and round buttons placed in touch units from a
 * corner) the engine lays out every frame and draws on top of the frame.
 *
 * Feel: a thumbstick's ring stays at its home. A touch landing anywhere in
 * its zone steers by the thumb's offset from HOME, so what is drawn is where
 * the stick is (mobile games' fixed stick with a large activation zone). The
 * floating flag keeps the older feel, the ring centred where the touch
 * landed, for games that want it. */
#include <math.h>
#include <string.h>

#include "cartwheel/core.h"
#include "cartwheel/debug.h"
#include "cartwheel/render2d.h"
#include "cartwheel/ui.h"
#include "input_internal.h"
#include "../platform/hooks.h"

#define I cw__in

static uint32_t overlay_drawn; /* debug field cw.touch_overlay: controls drawn last frame */

static bool valid_id(int id) { return I.init && id >= 0 && id < I.ncontrols; }

/* ── layout ───────────────────────────────────────────────────────── */

static void area_now(float *x, float *y, float *w, float *h) {
  if (I.area[2] > 0 && I.area[3] > 0) {
    *x = I.area[0], *y = I.area[1], *w = I.area[2], *h = I.area[3];
    return;
  }
  int ww = 0, wh = 0;
  cw_window_size(&ww, &wh);
  *x = 0, *y = 0, *w = (float)ww, *h = (float)wh;
}

/* corner-relative touch units (inward) -> window coordinates */
static cw_vec2 place(const cw__touch_control *c, float ax, float ay, float aw, float ah, float u, float cx, float cy) {
  bool right = c->corner == CW_TOUCH_BOTTOM_RIGHT || c->corner == CW_TOUCH_TOP_RIGHT;
  bool top = c->corner == CW_TOUCH_TOP_LEFT || c->corner == CW_TOUCH_TOP_RIGHT;
  return (cw_vec2){ right ? ax + aw - cx * u : ax + cx * u, top ? ay + cy * u : ay + ah - cy * u };
}

static void layout(void) {
  float ax, ay, aw, ah;
  area_now(&ax, &ay, &aw, &ah);
  float s = aw < ah ? aw : ah;
  float u = s > 0 ? s / CW_TOUCH_UNITS : 1.0f;
  I.unit = u;
  for (int i = 0; i < I.ncontrols; i++) {
    cw__touch_control *c = &I.controls[i];
    if (!c->anchored) continue;
    c->home = place(c, ax, ay, aw, ah, u, c->ux, c->uy);
    c->radius = c->ur * u;
    if (c->stick) { /* the zone: from the corner to `zone` past home on both axes */
      cw_vec2 a = place(c, ax, ay, aw, ah, u, 0, 0), b = place(c, ax, ay, aw, ah, u, c->ux + c->uzone, c->uy + c->uzone);
      c->x = fminf(a.x, b.x), c->y = fminf(a.y, b.y);
      c->w = fabsf(b.x - a.x), c->h = fabsf(b.y - a.y);
    } else { /* the hit circle's box (1.25 x the radius) */
      float r = c->radius * 1.25f;
      c->x = c->home.x - r, c->y = c->home.y - r, c->w = c->h = 2 * r;
    }
  }
}

/* ── adding and changing controls ─────────────────────────────────── */

static void overlay_draw(void);

static int add_control(cw__touch_control c) {
  cw__input_ensure_init();
  if (I.ncontrols >= CW_INPUT_MAX_TOUCH_CONTROLS) {
    cw_error("input: more than %d touch controls", CW_INPUT_MAX_TOUCH_CONTROLS);
    return -1;
  }
  I.controls[I.ncontrols] = c;
  int id = I.ncontrols++;
  if (c.anchored) {
    static bool hooked;
    if (!hooked) {
      hooked = true;
      cw__platform_hook_touch_overlay(overlay_draw);
      cw_debug_u32("cw.touch_overlay", &overlay_drawn);
    }
    layout();
    I.controls[id].center = I.controls[id].home;
  }
  return id;
}

static void apply_stick(cw__touch_control *c, const cw_touch_stick_desc *d) {
  cw_touch_stick_desc z = { 0 };
  if (!d) d = &z;
  c->stick = c->anchored = true;
  c->corner = (uint8_t)(d->corner == CW_TOUCH_CORNER_AUTO ? CW_TOUCH_BOTTOM_LEFT : d->corner);
  c->ux = d->x > 0 || d->y > 0 ? d->x : 150;
  c->uy = d->x > 0 || d->y > 0 ? d->y : 140;
  c->ur = d->radius > 0 ? d->radius : 70;
  c->uzone = d->zone > 0 ? d->zone : 2.4f * c->ur;
  c->floating = d->floating;
  c->hidden = d->hidden;
}

static void apply_button(cw__touch_control *c, const cw_touch_button_desc *d) {
  cw_touch_button_desc z = { 0 };
  if (!d) d = &z;
  c->stick = false;
  c->anchored = true;
  c->corner = (uint8_t)(d->corner == CW_TOUCH_CORNER_AUTO ? CW_TOUCH_BOTTOM_RIGHT : d->corner);
  c->ux = d->x > 0 || d->y > 0 ? d->x : 110;
  c->uy = d->x > 0 || d->y > 0 ? d->y : 110;
  c->ur = d->radius > 0 ? d->radius : 48;
  c->hidden = d->hidden;
  memset(c->label, 0, sizeof c->label);
  if (d->label) {
    size_t n = strlen(d->label);
    if (n >= sizeof c->label) n = sizeof c->label - 1;
    memcpy(c->label, d->label, n);
  }
  c->color = d->color.a > 0 ? d->color : cw_rgba(1, 1, 1, 1);
}

int cw_touch_add_thumbstick(const cw_touch_stick_desc *d) {
  cw__touch_control c = { 0 };
  apply_stick(&c, d);
  return add_control(c);
}
int cw_touch_add_round_button(const cw_touch_button_desc *d) {
  cw__touch_control c = { 0 };
  apply_button(&c, d);
  return add_control(c);
}
void cw_touch_set_thumbstick(int id, const cw_touch_stick_desc *d) {
  if (!valid_id(id) || !I.controls[id].anchored || !I.controls[id].stick) return;
  apply_stick(&I.controls[id], d);
  layout();
}
void cw_touch_set_round_button(int id, const cw_touch_button_desc *d) {
  if (!valid_id(id) || !I.controls[id].anchored || I.controls[id].stick) return;
  apply_button(&I.controls[id], d);
  layout();
}
void cw_touch_set_radius(int id, float radius) {
  if (!valid_id(id) || !(radius > 0)) return;
  cw__touch_control *c = &I.controls[id];
  if (c->anchored) {
    c->ur = radius;
    layout();
  } else if (c->stick)
    c->radius = radius;
}
void cw_touch_set_enabled(int id, bool on) {
  if (valid_id(id)) I.controls[id].disabled = !on;
}
void cw_touch_set_area(float x, float y, float w, float h) {
  cw__input_ensure_init();
  I.area[0] = x, I.area[1] = y, I.area[2] = w, I.area[3] = h;
  layout();
}
float cw_touch_unit(void) { return I.init && I.unit > 0 ? I.unit : 1.0f; }
void cw_touch_set_overlay(cw_touch_overlay_mode mode) {
  cw__input_ensure_init();
  I.overlay = (uint8_t)mode;
}
bool cw_touch_overlay_visible(void) {
  if (!I.init || I.overlay == CW_TOUCH_OVERLAY_OFF) return false;
  return I.overlay == CW_TOUCH_OVERLAY_ALWAYS || cw_platform_touch_primary() || I.touch_seen;
}

int cw_touch_add_button(float x, float y, float w, float h) {
  return add_control((cw__touch_control){ .x = x, .y = y, .w = w, .h = h });
}
int cw_touch_add_stick(float x, float y, float w, float h, float radius, bool floating) {
  return add_control((cw__touch_control){ .stick = true, .floating = floating, .x = x, .y = y, .w = w, .h = h,
                                          .radius = radius > 0 ? radius : 1 });
}
void cw_touch_set_rect(int id, float x, float y, float w, float h) {
  if (!valid_id(id) || I.controls[id].anchored) return;
  cw__touch_control *c = &I.controls[id];
  c->x = x, c->y = y, c->w = w, c->h = h;
}
void cw_touch_clear(void) {
  if (!I.init) return;
  I.ncontrols = 0;
  for (int i = 0; i < CW_MAX_TOUCHES; i++) I.touch_owner[i] = -1;
}
bool cw_touch_control_active(int id) { return valid_id(id) && I.controls[id].active; }
cw_vec2 cw_touch_stick_value(int id) { return valid_id(id) ? I.controls[id].value : (cw_vec2){ 0, 0 }; }
cw_vec2 cw_touch_stick_center(int id) { return valid_id(id) ? I.controls[id].center : (cw_vec2){ 0, 0 }; }
cw_vec2 cw_touch_home(int id) {
  if (!valid_id(id)) return (cw_vec2){ 0, 0 };
  const cw__touch_control *c = &I.controls[id];
  return c->anchored ? c->home : (cw_vec2){ c->x + c->w * 0.5f, c->y + c->h * 0.5f };
}
float cw_touch_radius(int id) {
  if (!valid_id(id)) return 0;
  const cw__touch_control *c = &I.controls[id];
  return c->anchored || c->stick ? c->radius : 0;
}

/* ── claiming and steering (every frame, before the bindings read them) ─ */

/* how well touch (x, y) fits control c: < 0 not at all; lower wins (ties: the earlier control) */
static float fit(const cw__touch_control *c, float x, float y) {
  if (c->disabled) return -1;
  if (!c->anchored) /* rect: first match wins, as before */
    return x >= c->x && x < c->x + c->w && y >= c->y && y < c->y + c->h ? 0 : -1;
  if (c->stick) /* the zone, behind any button in it */
    return x >= c->x && x < c->x + c->w && y >= c->y && y < c->y + c->h ? 2 : -1;
  float dx = x - c->home.x, dy = y - c->home.y, r = c->radius * 1.25f;
  float d2 = dx * dx + dy * dy;
  return d2 <= r * r ? sqrtf(d2) / r : -1; /* 0..1: the nearer centre wins */
}

void cw__touch_update(const cw_raw_input *raw) {
  layout();
  for (int c = 0; c < I.ncontrols; c++) {
    cw__touch_control *tc = &I.controls[c];
    tc->active = tc->tapped = false;
    tc->value = (cw_vec2){ 0, 0 };
    if (tc->anchored) tc->center = tc->home; /* a floating ring goes home when let go */
    else if (!tc->stick || !tc->floating) tc->center = (cw_vec2){ tc->x + tc->w * 0.5f, tc->y + tc->h * 0.5f };
  }
  for (int p = 0; p < CW_MAX_PADS; p++)
    if (raw->pads[p].pressed) I.touch_seen = false; /* a pad in use: the overlay (AUTO) steps aside */
  for (int i = 0; i < CW_MAX_TOUCHES; i++) {
    const cw_touch_state *t = &raw->touches[i];
    bool down = t->active || t->tapped;
    bool began = t->tapped || (t->active && !I.touch_prev_active[i]);
    if (began) {
      I.touch_seen = true;
      I.touch_owner[i] = -1;
      float best = 0;
      for (int c = 0; c < I.ncontrols; c++) {
        float f = fit(&I.controls[c], t->x, t->y);
        if (f >= 0 && (I.touch_owner[i] < 0 || f < best)) I.touch_owner[i] = (int8_t)c, best = f;
        if (f == 0) break;
      }
      int c = I.touch_owner[i];
      if (c >= 0) {
        cw__touch_control *tc = &I.controls[c];
        if (tc->stick && tc->floating) tc->start = (cw_vec2){ t->x, t->y };
      }
    }
    int c = I.touch_owner[i];
    if (down && c >= 0 && c < I.ncontrols && !I.controls[c].disabled) {
      cw__touch_control *tc = &I.controls[c];
      if (!tc->active) { /* the lowest touch slot steers */
        tc->active = true;
        if (tc->stick) {
          if (tc->floating) tc->center = tc->start;
          float dx = (t->x - tc->center.x) / tc->radius, dy = (t->y - tc->center.y) / tc->radius;
          float l = sqrtf(dx * dx + dy * dy);
          if (l > 1) dx /= l, dy /= l;
          tc->value = (cw_vec2){ dx, dy };
        }
      }
      if (t->tapped) tc->tapped = true;
    }
    if (!t->active) I.touch_owner[i] = -1;
    I.touch_prev_active[i] = t->active;
  }
}

/* ── the overlay ──────────────────────────────────────────────────── */

static void overlay_draw(void) {
  overlay_drawn = 0;
  if (!cw_touch_overlay_visible()) return;
  bool any = false;
  for (int i = 0; i < I.ncontrols; i++)
    if (I.controls[i].anchored && !I.controls[i].hidden && !I.controls[i].disabled) any = true;
  if (!any) return;
  cw_r2d_begin(NULL); /* a plain window pass: logical window coordinates, as touches are */
  float u = cw_touch_unit(), line = fmaxf(1.5f, 3.0f * u);
  for (int i = 0; i < I.ncontrols; i++) {
    const cw__touch_control *c = &I.controls[i];
    if (!c->anchored || c->hidden || c->disabled) continue;
    float r = c->radius;
    int seg = 48;
    if (c->stick) {
      cw_vec2 o = c->active ? c->center : c->home;
      cw_r2d_circle(o, r * 1.18f, seg, cw_rgba(0.02f, 0.02f, 0.04f, c->active ? 0.36f : 0.26f));
      cw_r2d_circle_lines(o, r, line, seg, cw_rgba(1, 1, 1, c->active ? 0.7f : 0.38f));
      cw_vec2 k = { o.x + c->value.x * r, o.y + c->value.y * r };
      cw_r2d_circle(k, r * 0.46f, seg, cw_rgba(0.02f, 0.02f, 0.04f, c->active ? 0.3f : 0.2f));
      cw_r2d_circle(k, r * 0.42f, seg, cw_rgba(1, 1, 1, c->active ? 0.78f : 0.42f));
    } else {
      cw_color f = c->color;
      cw_r2d_circle(c->home, r, seg, cw_rgba(0.02f, 0.02f, 0.04f, c->active ? 0.42f : 0.28f));
      cw_r2d_circle(c->home, r * 0.9f, seg, cw_rgba(f.r, f.g, f.b, c->active ? 0.62f : 0.26f));
      cw_r2d_circle_lines(c->home, r, line, seg, cw_rgba(1, 1, 1, c->active ? 0.75f : 0.4f));
      if (c->label[0]) {
        cw_font font = cw_ui_font_sans();
        float size = fminf(r * 0.62f, 34.0f * u * 1.2f);
        cw_text_style st = { .size = size, .color = cw_rgba(1, 1, 1, c->active ? 1.0f : 0.85f),
                             .shadow_color = cw_rgba(0, 0, 0, 0.5f), .shadow_offset = { 0, line * 0.5f } };
        cw_text_metrics m = cw_font_measure(font, c->label, &st);
        cw_r2d_text(font, c->label, (cw_vec2){ c->home.x - m.width * 0.5f, c->home.y - m.height * 0.5f }, &st);
      }
    }
    overlay_drawn++;
  }
  cw_r2d_end();
}

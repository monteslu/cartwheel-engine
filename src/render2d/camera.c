/* render2d: affine helpers and Camera2D. Deterministic math (cw_sincos,
 * cw_exp) so game logic may use screen_to_world and follow. */
#include "internal.h"

cw_xform2d cw_xform2d_mul(const cw_xform2d *m, const cw_xform2d *n) {
  return (cw_xform2d){
    m->a * n->a + m->c * n->b,
    m->b * n->a + m->d * n->b,
    m->a * n->c + m->c * n->d,
    m->b * n->c + m->d * n->d,
    m->a * n->tx + m->c * n->ty + m->tx,
    m->b * n->tx + m->d * n->ty + m->ty,
  };
}

cw_xform2d cw_xform2d_invert(const cw_xform2d *m) {
  float det = m->a * m->d - m->b * m->c;
  if (det == 0) return cw_xform2d_identity();
  float id = 1.0f / det;
  cw_xform2d r = { m->d * id, -m->b * id, -m->c * id, m->a * id, 0, 0 };
  r.tx = -(r.a * m->tx + r.c * m->ty);
  r.ty = -(r.b * m->tx + r.d * m->ty);
  return r;
}

cw_xform2d cw_xform2d_trs(cw_vec2 pos, float rot, cw_vec2 scale) {
  float s = 0, c = 1;
  if (rot != 0) cw_sincos(rot, &s, &c);
  return (cw_xform2d){ c * scale.x, s * scale.x, -s * scale.y, c * scale.y, pos.x, pos.y };
}

cw_xform2d cw_camera2d_view(const cw_camera2d *cam, cw_rect vp, cw_vec2 parallax) {
  float z = cam->zoom != 0 ? cam->zoom : 1.0f;
  float s = 0, c = 1;
  if (cam->rotation != 0) cw_sincos(cam->rotation, &s, &c);
  if (parallax.x == 0 && parallax.y == 0) parallax = (cw_vec2){ 1, 1 };
  cw_vec2 anchor = { vp.x + cam->offset.x, vp.y + cam->offset.y };
  if (!cam->anchor_top_left) {
    anchor.x += vp.w * 0.5f;
    anchor.y += vp.h * 0.5f;
  }
  cw_xform2d m = { z * c, -z * s, z * s, z * c, 0, 0 };
  float px = cam->position.x * parallax.x, py = cam->position.y * parallax.y;
  m.tx = anchor.x - (m.a * px + m.c * py);
  m.ty = anchor.y - (m.b * px + m.d * py);
  return m;
}

cw_vec2 cw_camera2d_world_to_screen(const cw_camera2d *cam, cw_rect vp, cw_vec2 w) {
  cw_xform2d m = cw_camera2d_view(cam, vp, (cw_vec2){ 1, 1 });
  return cw_xform2d_apply(&m, w);
}

cw_vec2 cw_camera2d_screen_to_world(const cw_camera2d *cam, cw_rect vp, cw_vec2 p) {
  cw_xform2d m = cw_camera2d_view(cam, vp, (cw_vec2){ 1, 1 });
  cw_xform2d inv = cw_xform2d_invert(&m);
  return cw_xform2d_apply(&inv, p);
}

void cw_camera2d_apply_limits(cw_camera2d *cam, cw_rect vp) {
  if (!cam->use_limits) return;
  float z = cam->zoom != 0 ? cam->zoom : 1.0f;
  float hw = vp.w / (2 * z), hh = vp.h / (2 * z);
  float ox = cam->anchor_top_left ? hw : 0, oy = cam->anchor_top_left ? hh : 0; /* position -> view centre */
  float cx = cam->position.x + ox, cy = cam->position.y + oy;
  const cw_rect *L = &cam->limits;
  cx = L->w <= 2 * hw ? L->x + L->w * 0.5f : cw_clampf(cx, L->x + hw, L->x + L->w - hw);
  cy = L->h <= 2 * hh ? L->y + L->h * 0.5f : cw_clampf(cy, L->y + hh, L->y + L->h - hh);
  cam->position = (cw_vec2){ cx - ox, cy - oy };
}

void cw_camera2d_follow(cw_camera2d *cam, cw_vec2 target, float speed, float dt) {
  if (speed <= 0) {
    cam->position = target;
  } else {
    float k = 1.0f - cw_exp(-speed * dt);
    cam->position.x += (target.x - cam->position.x) * k;
    cam->position.y += (target.y - cam->position.y) * k;
  }
}

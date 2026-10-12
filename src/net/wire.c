/* cw_value on the wire: [u8 type][payload]. Node values are paths relative to
 * the session root, so they name the same node on every peer. */
#include "net_internal.h"
#include "cartwheel/core.h"

const char *cw__r_str(cw__rbuf *r) {
  int n = cw__r_u16(r);
  if (r->bad || r->pos + n > r->len) {
    r->bad = true;
    return NULL;
  }
  /* frame-scoped: names and paths are looked up and dropped (a peer's
   * per-spawn names must not pile up); a value's string is pinned below */
  char *s = cw_frame_alloc((size_t)n + 1);
  memcpy(s, r->p + r->pos, (size_t)n);
  s[n] = 0;
  r->pos += n;
  return s;
}

void cw__w_value(cw__wbuf *w, const cw_value *v, cw_node root) {
  cw__w_u8(w, (uint8_t)v->type);
  switch (v->type) {
  case CW_T_BOOL: cw__w_u8(w, v->b); break;
  case CW_T_INT: case CW_T_ENUM: cw__w_u64(w, (uint64_t)v->i); break;
  case CW_T_FLOAT: cw__w_f64(w, v->f); break;
  case CW_T_VEC2: for (int i = 0; i < 2; i++) cw__w_f32(w, v->v[i]); break;
  case CW_T_VEC3: for (int i = 0; i < 3; i++) cw__w_f32(w, v->v[i]); break;
  case CW_T_VEC4: case CW_T_QUAT: case CW_T_COLOR: for (int i = 0; i < 4; i++) cw__w_f32(w, v->v[i]); break;
  case CW_T_STRING: cw__w_str(w, v->s); break;
  case CW_T_NODE: {
    char path[512] = "";
    if (cw_node_valid(v->node)) cw_node_path(v->node, root, path, sizeof path);
    cw__w_u8(w, cw_node_valid(v->node));
    cw__w_str(w, path);
    break;
  }
  case CW_T_RES: cw__w_u64(w, v->res); break;
  default: break;
  }
}

bool cw__r_value(cw__rbuf *r, cw_value *v, cw_node root) {
  memset(v, 0, sizeof *v);
  v->type = (cw_type)cw__r_u8(r);
  switch (v->type) {
  case CW_T_NONE: break;
  case CW_T_BOOL: v->b = cw__r_u8(r) != 0; break;
  case CW_T_INT: case CW_T_ENUM: v->i = (int64_t)cw__r_u64(r); break;
  case CW_T_FLOAT: v->f = cw__r_f64(r); break;
  case CW_T_VEC2: for (int i = 0; i < 2; i++) v->v[i] = cw__r_f32(r); break;
  case CW_T_VEC3: for (int i = 0; i < 3; i++) v->v[i] = cw__r_f32(r); break;
  case CW_T_VEC4: case CW_T_QUAT: case CW_T_COLOR: for (int i = 0; i < 4; i++) v->v[i] = cw__r_f32(r); break;
  case CW_T_STRING: {
    const char *s = cw__r_str(r);
    v->s = s ? cw_intern(s) : NULL;
    break;
  }
  case CW_T_NODE: {
    bool valid = cw__r_u8(r) != 0;
    const char *path = cw__r_str(r);
    if (valid && path) v->node = path[0] ? cw_node_get(root, path) : root;
    break;
  }
  case CW_T_RES: v->res = cw__r_u64(r); break;
  default: r->bad = true; break;
  }
  return !r->bad;
}

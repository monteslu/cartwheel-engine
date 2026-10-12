/* Resource groups: a set of loads started together and watched as one (a
 * level's preload list, a loading screen). */
#include <stdio.h>
#include <stdlib.h>

#include "cartwheel/platform.h"
#include "resource_internal.h"

struct cw_res_group {
  CW_ARRAY(cw_res) res;
};

cw_res_group *cw_res_group_new(void) { return cw_calloc(1, sizeof(cw_res_group), CW_MEM_RESOURCE); }

cw_res cw_res_group_add(cw_res_group *g, const char *path, cw_res_type type) {
  cw_res r = cw_res_load_async(path, type, NULL, NULL);
  cw_array_push(&g->res, r);
  return r;
}

int cw_res_preload(cw_res_group *g, const char *scene_path) {
  char c[CW_RES_PATH_MAX], list[CW_RES_PATH_MAX + 16];
  if (!cw__res_canon(scene_path, c, sizeof c)) {
    cw_error("cw_res_preload: \"%s\" is not a resource path", scene_path);
    return 0;
  }
  snprintf(list, sizeof list, "%s.preload", c);
  size_t len;
  char *t = cw_asset_load(list, &len);
  if (!t) { /* not packed by cwpack (or not a scene): the file alone */
    cw_res_group_add(g, c, 0);
    return 1;
  }
  int n = 0;
  for (char *line = t; line && *line;) {
    char *nl = strchr(line, '\n');
    if (nl) *nl = 0;
    size_t l = strlen(line);
    while (l && (line[l - 1] == '\r' || line[l - 1] == ' ')) line[--l] = 0;
    if (l && line[0] != '#') { cw_res_group_add(g, line, 0); n++; }
    line = nl ? nl + 1 : NULL;
  }
  free(t);
  return n;
}

void cw_res_group_get_progress(const cw_res_group *g, cw_res_group_progress *o) {
  memset(o, 0, sizeof *o);
  o->total = g->res.count;
  for (int i = 0; i < g->res.count; i++) {
    cw_res_info info;
    cw_res_get_info(g->res.data[i], &info);
    switch (info.state) {
    case CW_RES_STATE_READY: o->ready++; o->bytes += info.bytes; o->file_bytes += (int64_t)info.file_bytes; break;
    case CW_RES_STATE_FAILED: o->failed++; break;
    default: o->loading++; break;
    }
  }
  o->fraction = o->total ? (float)(o->ready + o->failed) / (float)o->total : 1.0f;
}

bool cw_res_group_done(const cw_res_group *g) {
  for (int i = 0; i < g->res.count; i++) {
    cw_res_state s = cw_res_get_state(g->res.data[i]);
    if (s == CW_RES_STATE_LOADING) return false;
  }
  return true;
}

void cw_res_group_wait(cw_res_group *g) {
  for (int i = 0; i < g->res.count; i++) cw_res_wait(g->res.data[i]);
}

int cw_res_group_count(const cw_res_group *g) { return g->res.count; }
cw_res cw_res_group_at(const cw_res_group *g, int i) {
  return i >= 0 && i < g->res.count ? g->res.data[i] : (cw_res){ 0 };
}

void cw_res_group_free(cw_res_group *g) {
  if (!g) return;
  for (int i = 0; i < g->res.count; i++) cw_res_release(g->res.data[i]);
  cw_array_free(&g->res);
  cw_free(g);
}

/* Physics node project settings: layer names, gravity, scale (see
 * cartwheel/physics_nodes.h), read once from the optional project.cwproj. */
#include <stdio.h>
#include <stdlib.h>

#include "scene_internal.h"
#include "cartwheel/physics_nodes.h"
#include "cartwheel/platform.h"
#include "cartwheel/vfs.h"

static cw_physics_nodes_config cfg;
static bool loaded;

static void names_into(const char *list, const char **out) {
  int k = 0;
  const char *p = list;
  while (*p && k < CW_PHYS_LAYERS) {
    while (*p == ' ' || *p == ',' || *p == '|') p++;
    const char *s = p;
    while (*p && *p != ',' && *p != '|') p++;
    const char *e = p;
    while (e > s && e[-1] == ' ') e--;
    out[k++] = e > s ? cw_intern_n(s, (size_t)(e - s)) : NULL;
  }
}

static void load(void) {
  if (loaded) return;
  loaded = true;
  cfg.gravity_2d = cw_v2(0, 980);
  cfg.gravity_3d = (t3_vec3){ 0, -9.8f, 0 };
  cfg.pixels_per_meter = 32;
  cfg.substeps = 4;
  size_t len;
  /* through the project layer (cartwheel/vfs.h): an editor opened on a project
   * directory resolves that project's layer names */
  char *text = cw_vfs_read("project.cwproj", &len);
  if (!text) return;
  for (char *line = strtok(text, "\n"); line; line = strtok(NULL, "\n")) {
    char *eq = strchr(line, '=');
    if (!eq) continue;
    char key[64];
    int kl = 0;
    for (char *k = line; k < eq && kl < 63; k++)
      if (*k != ' ' && *k != '\t') key[kl++] = *k;
    key[kl] = 0;
    cw_value v;
    if (!cw_value_parse(eq + 1, CW_T_NONE, &v)) continue;
    if (!strcmp(key, "layer_names_2d") && v.type == CW_T_STRING) names_into(v.s, cfg.layers_2d);
    else if (!strcmp(key, "layer_names_3d") && v.type == CW_T_STRING) names_into(v.s, cfg.layers_3d);
    else if (!strcmp(key, "gravity_2d") && v.type == CW_T_VEC2) cfg.gravity_2d = cw_v2(v.v[0], v.v[1]);
    else if (!strcmp(key, "gravity_3d") && v.type == CW_T_VEC3) cfg.gravity_3d = (t3_vec3){ v.v[0], v.v[1], v.v[2] };
    else if (!strcmp(key, "pixels_per_meter")) cfg.pixels_per_meter = (float)(v.type == CW_T_INT ? (double)v.i : v.f);
    else if (!strcmp(key, "substeps") && v.type == CW_T_INT) cfg.substeps = (int)v.i;
  }
  free(text);
}

const cw_physics_nodes_config *cw_physics_nodes_get_config(void) {
  load();
  return &cfg;
}

void cw_physics_nodes_config_set(const cw_physics_nodes_config *c) {
  load();
  cfg = *c;
  for (int i = 0; i < CW_PHYS_LAYERS; i++) {
    if (cfg.layers_2d[i]) cfg.layers_2d[i] = cw_intern(cfg.layers_2d[i]);
    if (cfg.layers_3d[i]) cfg.layers_3d[i] = cw_intern(cfg.layers_3d[i]);
  }
  if (cfg.pixels_per_meter <= 0) cfg.pixels_per_meter = 32;
  if (cfg.substeps <= 0) cfg.substeps = 4;
}

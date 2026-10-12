/* cwbake: bake a scene's lighting offline (cartwheel/lightmap.h).
 *
 *   cwbake <scene.cwscene> <out.cwlm> [--seed N]
 *
 * Loads the scene into a tree (no window, no GL), runs one pre_render so
 * every node writes its transform and settings (PostProcess mode, the
 * ProceduralSky's sun), bakes with the settings of the scene's LightmapGI
 * node (its parent's subtree; the whole scene and the defaults when it has
 * none) on the job pool, writes the .cwlm and prints its fingerprint. The
 * bytes equal what the LightmapGI node bakes at runtime from the same scene,
 * on any host; ship the file and point LightmapGI.data at it. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cartwheel/jobs.h"
#include "cartwheel/lightmap.h"
#include "cartwheel/render3d.h"
#include "cartwheel/scene_file.h"

static bool find_gi(cw_node n, void *u) {
  cw_node *out = u;
  if (CW_HANDLE_IS_NULL(*out) && cw_node_is(n, "LightmapGI")) *out = n;
  return true;
}

static float fprop(cw_node n, const char *name, float d) {
  cw_value v;
  return cw_node_get_prop(n, name, &v) ? (v.type == CW_T_INT ? (float)v.i : (float)v.f) : d;
}

int main(int argc, char **argv) {
  if (argc < 3) {
    fprintf(stderr, "usage: cwbake <scene.cwscene> <out.cwlm> [--seed N]\n");
    return 2;
  }
  const char *in = argv[1], *out = argv[2];
  long seed = -1;
  for (int i = 3; i < argc; i++)
    if (!strcmp(argv[i], "--seed") && i + 1 < argc) seed = strtol(argv[++i], NULL, 0);
  FILE *f = fopen(in, "rb");
  if (!f) {
    fprintf(stderr, "cwbake: cannot read %s\n", in);
    return 1;
  }
  fseek(f, 0, SEEK_END);
  long len = ftell(f);
  fseek(f, 0, SEEK_SET);
  char *text = malloc((size_t)len + 1);
  size_t got = fread(text, 1, (size_t)len, f);
  fclose(f);
  text[got] = 0;
  cw_jobs_init(-1);
  cw_scene_register_builtin_types();
  cw_tree *t = cw_tree_new();
  cw_set_current_tree(t);
  cw_packed_scene *ps = cw_scene_parse(text, in);
  free(text);
  if (!ps) {
    fprintf(stderr, "cwbake: %s did not parse\n", in);
    return 1;
  }
  cw_node scene = cw_scene_instance(ps);
  cw_node_add_child(cw_tree_root(t), scene);
  cw_tree_pre_render(t, 1.0);
  cw_node gi = { 0 }, root = scene;
  cw_node_walk(scene, find_gi, &gi);
  cw_lightmap_settings s = cw_lightmap_settings_default();
  if (!CW_HANDLE_IS_NULL(gi)) {
    root = cw_node_parent(gi);
    s.texel_density = fprop(gi, "texel_density", s.texel_density);
    s.min_size = (int)fprop(gi, "min_size", (float)s.min_size);
    s.max_size = (int)fprop(gi, "max_size", (float)s.max_size);
    s.padding = (int)fprop(gi, "padding", (float)s.padding);
    s.samples = (int)fprop(gi, "samples", (float)s.samples);
    s.bounces = (int)fprop(gi, "bounces", (float)s.bounces);
    s.seed = (uint64_t)(uint32_t)(int)fprop(gi, "seed", 1);
    s.probe_spacing = fprop(gi, "probe_spacing", s.probe_spacing);
    s.probe_samples = (int)fprop(gi, "probe_samples", (float)s.probe_samples);
    s.intensity = fprop(gi, "intensity", s.intensity);
    s.denoise = (int)fprop(gi, "denoise", (float)s.denoise);
  }
  if (seed >= 0) s.seed = (uint64_t)seed;
  cw_lightmap_data *d = cw_lightmap_bake(t, root, &s);
  if (!d) return 1;
  bool ok = cw_lightmap_save(d, out);
  cw_lightmap_info info;
  cw_lightmap_get_info(d, &info);
  printf("{\"hash\":\"%016llx\",\"meshes\":%d,\"triangles\":%d,\"texels\":%d,\"probes\":%d,\"invalid_probes\":%d,\"rays\":%lld}\n",
         (unsigned long long)info.hash, info.meshes, info.triangles, info.texels, info.probes, info.invalid_probes, info.rays);
  cw_lightmap_free(d);
  cw_tree_free(t);
  return ok ? 0 : 1;
}

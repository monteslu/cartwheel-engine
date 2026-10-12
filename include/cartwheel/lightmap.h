/* Baked lighting: an offline CPU lightmapper and an ambient probe
 * grid.
 *
 * What is baked. MeshInstance3D nodes with gi_mode STATIC are the baked
 * geometry: each gets a second UV set (generated: triangles flood-filled
 * into charts of similar normal, each chart projected onto its plane at
 * texel_density texels per world unit x the mesh's lightmap_scale, charts
 * shelf-packed with `padding` texels between them) and a lightmap of its own.
 * The lightmap holds the INDIRECT irradiance: sky light (the WorldEnvironment
 * ambient + hemisphere light, or the ProceduralSky's radiance when the tree
 * has one) and `bounces` bounces of every Light3D with `bake` (default on),
 * the light's direct part shadow-tested against the baked geometry. Direct
 * light stays real-time (engine or three.c lights, with their shadows), so
 * moving objects still cast and lights can still change; the baked part is
 * what real-time lighting cannot do: occluded sky and colour bleeding.
 * Lightmapped meshes take that irradiance in place of the ambient,
 * hemisphere and environment-diffuse light (three.js's lightMap, read
 * through uv1; on r186 programs once three.c reads uv1, see lighting.h). Everything else (moving objects, DYNAMIC meshes) inside the baked
 * volume takes its ambient light from a grid of L2 spherical-harmonic probes
 * (probe_spacing apart over the static geometry's bounds), interpolated per
 * pixel. Standard and Phong materials use both (Lambert and Basic read
 * lightmaps through three.c alone).
 *
 * How. A BVH over the static triangles (median splits on the longest axis,
 * deterministic), cosine-weighted hemisphere rays per texel (`samples` per
 * bounce), one shadow ray per light per texel, bounce radiance looked up in
 * the previous bounce's lightmaps, the sky through a 64 x 32 table of the
 * sky model. Texels run in parallel on the job pool, each with its own
 * random sequence seeded from (seed, mesh, texel), all math scalar IEEE
 * (cw_ trig), so the same input gives the same bytes on every host and any
 * thread count: cw_lightmap_hash over the serialized data is the bake's
 * fingerprint. The Monte Carlo noise is smoothed per chart (texels whose
 * normals agree and whose positions are near, `denoise` texels around).
 * Gaps between charts are filled from neighbouring texels
 * (dilation) so bilinear filtering never reads black. Units are physical
 * (three.js r186's); a file baked in r128's legacy units says so on load.
 *
 * Node: LightmapGI (docs/nodes.json) bakes on its first frame (or loads
 * `data`, a .cwlm asset written by cw_lightmap_save) and applies the result
 * to the tree. A bake is synchronous: tools and editors bake ahead and ship
 * the .cwlm. */
#ifndef CW_LIGHTMAP_H
#define CW_LIGHTMAP_H

#include "scene.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct cw_lightmap_settings {
  float texel_density;      /* 8 texels per world unit */
  int min_size, max_size;   /* 8, 1024: a mesh's lightmap side, texels */
  int padding;              /* 2 texels around each chart */
  int samples;              /* 64 hemisphere rays per texel per bounce */
  int bounces;              /* 2 */
  uint64_t seed;            /* 1 */
  float probe_spacing;      /* 2 world units between SH probes; 0 = no probe grid */
  int probe_samples;        /* 128 rays per probe */
  float intensity;          /* 1: multiplier of the baked light at runtime */
  int denoise;              /* 2: edge-aware smoothing radius, texels (0 = none) */
} cw_lightmap_settings;

/* The defaults above. */
cw_lightmap_settings cw_lightmap_settings_default(void);

typedef struct cw_lightmap_data cw_lightmap_data;

typedef struct cw_lightmap_info {
  int meshes, triangles, texels, probes, invalid_probes;
  int width_total;          /* sum of the lightmaps' widths (a size fingerprint) */
  long long rays;
  uint64_t hash;            /* cw_lightmap_hash */
} cw_lightmap_info;

/* Bake the tree under `root` (its STATIC MeshInstance3D nodes, its Light3D
 * nodes with `bake`, its sky). Synchronous, job-parallel. World transforms
 * are taken as they are now (call after the tree's first frame, or after
 * cw_tree_pre_render). Returns NULL (logged) when there is nothing to bake. */
cw_lightmap_data *cw_lightmap_bake(cw_tree *t, cw_node root, const cw_lightmap_settings *s);
void cw_lightmap_free(cw_lightmap_data *d); /* frees a baked, parsed or loaded lightmap; NULL is a no-op */
/* The serialized bytes (what cw_lightmap_save writes), owned by d. */
const void *cw_lightmap_bytes(const cw_lightmap_data *d, size_t *len);
/* FNV-1a 64 over the serialized bytes: equal bakes, equal hashes. */
uint64_t cw_lightmap_hash(const cw_lightmap_data *d);
/* copies the header (sizes, counts); d must not be NULL */
void cw_lightmap_get_info(const cw_lightmap_data *d, cw_lightmap_info *out);
/* Native: write the bytes to a file (false and logged on failure). */
bool cw_lightmap_save(const cw_lightmap_data *d, const char *path);
/* Parse bytes / load an asset (cw_asset_load). NULL (logged) when invalid. */
cw_lightmap_data *cw_lightmap_parse(const void *bytes, size_t len);
cw_lightmap_data *cw_lightmap_load(const char *asset_path); /* caller frees with cw_lightmap_free */
/* Give the tree the result: uv1 and lightmaps (the material's lightMap) to
 * the baked meshes (matched by node path under root; a mesh whose geometry
 * changed since the bake is skipped with a warning). d must outlive the
 * application (the LightmapGI node keeps it). */
void cw_lightmap_apply(cw_tree *t, cw_node root, const cw_lightmap_data *d, float intensity);

#ifdef __cplusplus
}
#endif
#endif

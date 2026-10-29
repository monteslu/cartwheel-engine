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
 * lightmaps through three.c alone). The probe grid is baked and stored in the
 * .cwlm; no material reads it yet.
 *
 * Which UVs. three.c's programs read every map, the lightmap included,
 * through uv0 (r186 samples a texture through uv(texture.channel), and three.c
 * has no texture channel yet): the generated uv1 charts are set on the mesh
 * but not read, so a lightmap with GENERATED charts lands where the mesh's
 * uv0 puts it. A mesh whose uv0 covers [0, 1] without overlapping (a ground
 * plane, a terrain, a hand-unwrapped level) sets MeshInstance3D lightmap_uv
 * UV0: the bake lays its lightmap on that uv0 (a square map of about
 * texel_density texels a unit of its area), and it lines up on GL and WebGPU.
 *
 * Fully baked lighting (opt in; nothing changes unless a scene asks). A
 * light with bake_direct puts its direct light (shadowed by the static
 * geometry, soft when shadow_softness > 0) into the lightmaps as well, and
 * LightmapGI's shading BAKED draws every lightmapped mesh unlit: a Basic
 * copy of its material (colour, map, vertex colours, AO map, transparency)
 * whose light is all from its lightmap (albedo x irradiance / PI, the same
 * Lambert diffuse three.js's lightMap gives a Standard material). No
 * real-time light reaches those meshes, so their cost per pixel is a few
 * texture reads however many lights the scene has; the lights still light
 * everything else (moving objects, DYNAMIC meshes) in real time. This is how
 * games on weak GPUs light static scenery: the floor of a big lit yard costs
 * the same with one light or twenty.
 *
 * A light with realtime off is in the bake only (its lightmap light and its
 * bounces); it lights nothing at run time and costs nothing a frame. Moving
 * objects can still take it from the ground: cw_lightmap_irradiance_below.
 *
 * Instanced scenery: a MeshInstance3D whose procedural mesh
 * (cw_mesh3d_set_procedural) is a t3_instanced_mesh, with gi_mode STATIC,
 * occludes and bounces light in the bake (every instance) but gets no
 * lightmap (its instances share one geometry) and stays lit in real time.
 *
 * Reflection cube: reflection_size > 0 traces a cube map from
 * reflection_origin through the baked scene (each texel the radiance the bake
 * left on what it sees, the sky where it sees nothing; emissive surfaces
 * glow in it). cw_lightmap_use_reflection gives it to a material as its
 * envMap: wet patches and puddles reflect the lit scene for one texture read,
 * where screen-space reflections cost a G-buffer and a trace every frame.
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
  int direct_samples;       /* 1: shadow rays a texel per light (more with shadow_softness > 0) */
  float shadow_softness;    /* 0 (hard): the light's radius over its distance (a directional light's angular
                             * radius, radians) for soft baked shadows */
  int reflection_size;      /* 0: no reflection cube; else its face side, texels (4 .. 256) */
  float reflection_origin[3]; /* (0, 1.5, 0): where the cube is traced from, world */
} cw_lightmap_settings;

/* How lightmapped meshes draw: LIT keeps their material (the lightmap is
 * their indirect light, real-time lights add the direct); BAKED draws them
 * unlit from the lightmap alone (bake the direct light: bake_direct). */
typedef enum { CW_LIGHTMAP_LIT, CW_LIGHTMAP_BAKED } cw_lightmap_shading;

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
/* The same, choosing how the meshes draw (cw_lightmap_apply is LIT). */
void cw_lightmap_apply_shaded(cw_tree *t, cw_node root, const cw_lightmap_data *d, float intensity,
                              cw_lightmap_shading shading);
/* The bake's reflection cube (NULL when it has none), made on first call and
 * owned by d: 8-bit sRGB, scaled down by *scale (the brightest texel). */
t3_texture *cw_lightmap_reflection(const cw_lightmap_data *d, float *scale);
/* Give material m the reflection cube as its envMap at strength (1 = the
 * baked radiance): Standard / Physical through envMapIntensity, Basic,
 * Lambert and Phong added on top (combine ADD, reflectivity). false when the
 * bake has no cube. */
bool cw_lightmap_use_reflection(const cw_lightmap_data *d, t3_material *m, float strength);
/* The baked irradiance (linear, physical units: what the lightmap holds) on
 * the highest upward-facing lightmapped surface at or below pos, read from
 * its lightmap (bilinear). A light probe from the floor, for things that
 * move over a baked ground: a character in a floodlight's pool, or in a
 * wall's shadow, takes that light, for no GPU cost (a top-down or racing game
 * lights its crowd or its cars this way). Needs d applied (the meshes' world
 * triangles are taken then; a mesh moved after the apply is not followed).
 * false (and black) where no baked surface lies below. */
bool cw_lightmap_irradiance_below(const cw_lightmap_data *d, t3_vec3 pos, t3_color *out);
/* A LightmapGI node's bake once it has baked or loaded (NULL before, and for
 * another node): for cw_lightmap_reflection, cw_lightmap_use_reflection and
 * cw_lightmap_irradiance_below. Owned by the node. */
const cw_lightmap_data *cw_lightmap_gi_data(cw_node gi);

#ifdef __cplusplus
}
#endif
#endif

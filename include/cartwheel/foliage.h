/* cartwheel/foliage.h: grass, leaves and reeds that look like plants:
 * wind on the vertices, smooth cut-out edges, two-sided lighting and
 * dithered fades. Shading parts (cartwheel/shading.h) on a material.
 *
 * Wind. Each vertex leans along the wind by its height on its mesh: none at
 * the root (y = 0 of the mesh), `strength` metres at `height` and above,
 * growing with the square of the height so the stem bends rather than
 * tilts. The lean comes and goes: a sway at `frequency` whose phase is
 * taken from the vertex's world position (a smooth noise about a metre
 * across), so neighbouring plants never sway in lockstep, plus `gust`, a
 * share of the strength that travels over the field along the wind in
 * fronts `gust_scale` metres apart. The lean stays between none and
 * `strength` downwind, with a little flutter across the wind (an eighth of
 * the strength). The clock is the engine's frame clock (the rendered
 * frames' cw_frame_dt summed, snapped to the fixed step in test runs), so
 * the same frames draw the same leaves on every host, in every view of a
 * split screen. The shadow follows the leaves (the caster program moves
 * them too). An instance's scale scales its lean. The velocity pass does not
 * see the wind (TAA reprojects the plant as still, which at a sway's speed
 * shows no ghost).
 *
 * Smooth cut-out edges (smooth_alpha, materials with alpha_test above 0: a
 * glTF material with alphaMode MASK, leaf and grass cards). A plain alpha
 * test keeps a pixel or drops it, so a card's edge stair-steps, and as the
 * texture's mip levels average its alpha the thin parts drop under the
 * cutoff and flicker as the camera moves. With smooth_alpha:
 *   - under MSAA (PostProcess msaa) the alpha becomes the pixel's sample
 *     coverage (alpha to coverage, three.js r187's material.alphaToCoverage:
 *     the alpha sharpened over a pixel around the cutoff), so the edge is
 *     antialiased by the MSAA samples;
 *   - without MSAA each pixel is kept with a probability that rises with its
 *     alpha (half at the cutoff, all at twice it), against the engine's
 *     interleaved gradient noise moved every frame under TAA (hashed alpha:
 *     TAA averages the frames into a soft edge that keeps the card's
 *     coverage at every mip level). Without TAA the noise holds still
 *     (grain on the edges, no flicker).
 * The engine picks the mode each frame from the scene pass's sample count.
 *
 * Two-sided lighting (two_sided_lighting). A double-sided material flips
 * its normal on back faces (three.js r187), so a blade or leaf whose normals
 * are bent up for soft lighting goes dark on its back. With this set the
 * back face is lit with the front face's normal (the normal map's too): the
 * leaf reads as one thin sheet lit the same from both sides.
 *
 * Dithered fades (fade_out_from / fade_out_to and fade_in_from / fade_in_to,
 * metres from the camera): pixels are dropped with a probability that rises
 * across the band (a screen-door dither on the engine's interleaved gradient
 * noise, moved every frame under TAA so it resolves to a fade). Two LODs
 * given the same band, one fading out and one fading in, draw complementary
 * pixels: a crossfade with no pop. TerrainScatter3D's DITHER fade mode sets
 * these on its own copies of the layer's materials.
 *
 * Root colour (root_color): the lower part of the mesh (to root_height
 * metres) takes the instance colour of an InstancedMesh (t3_instanced_mesh's
 * instance_color) instead of the material's colour; above it the material's
 * colour, its map and vertex colours as usual, varied per instance by up to
 * tip_jitter. TerrainScatter3D's root_from_terrain writes the terrain's
 * colour under each instance there, so grass grows out of the ground in its
 * colour.
 *
 * Per material: every mesh drawing the material gets the same values (clone
 * a material for different ones, t3_material_clone). Changing them is free
 * (uniforms per draw); the parts are added once. Works on
 * MeshStandardMaterial, MeshPhysicalMaterial, MeshLambertMaterial and
 * MeshPhongMaterial, instanced or not, with every other part. Cost on the
 * Radeon 890M: see the terrain guide's foliage section. A material with no
 * foliage draws exactly as before.
 *
 * Program tables: a material with foliage draws the composed state with
 * "foliage" (and "folalpha" for smooth_alpha on an alpha-tested material) in
 * its extension; tools/programs.mjs finds those like any other state. */
#ifndef CW_FOLIAGE_H
#define CW_FOLIAGE_H

#include "base.h"
#include "three.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct cw_wind {
  float strength;       /* metres the top of the mesh leans at most (0: no wind) */
  float frequency;      /* sways a second (0.5) */
  float direction[2];   /* world x, z the wind blows toward (1, 0; normalised when used) */
  float gust;           /* 0..1: the share of the strength that comes in gusts travelling over the field (0.5) */
  float gust_scale;     /* metres between gust fronts (14) */
} cw_wind;

typedef struct cw_foliage_desc {
  cw_wind wind;
  float height;             /* metres from the mesh's root (y 0) to where the full lean is reached; 0 = the
                             * geometry's bounding box top (cw_foliage_apply_mesh) or 1 */
  bool smooth_alpha;        /* alpha to coverage under MSAA, hashed alpha without (alpha-tested materials) */
  bool two_sided_lighting;  /* back faces lit with the front face's normal */
  float fade_in_from, fade_in_to;   /* metres: pixels appear across this band (to <= from: none) */
  float fade_out_from, fade_out_to; /* metres: pixels vanish across this band (to <= from: none) */
  bool root_color;          /* the lower part takes the instance colour (an InstancedMesh's instance_color) */
  float root_height;        /* metres the root colour reaches (0.6 of height when 0) */
  float tip_jitter;         /* 0..1: per-instance variation of the upper colour under root_color (0) */
} cw_foliage_desc;

/* No wind, nothing on: a desc that changes nothing. */
cw_foliage_desc cw_foliage_default(void);
/* A breeze: strength 0.08, frequency 0.5, direction (1, 0), gust 0.5, gust_scale 14. */
cw_wind cw_wind_default(void);

/* Give material m foliage (d NULL: cw_foliage_default). false for a
 * material type the parts do not take, or when m would carry more than
 * CW_SHADING_MAX_PARTS parts. Applying again replaces the values (cheap: a
 * game may change the wind every frame). smooth_alpha also sets three.c's
 * alpha_to_coverage on m. The material is retained until cw_foliage_remove. */
bool cw_foliage_apply(t3_material *m, const cw_foliage_desc *d);
/* The same with height 0 taken from g's bounding box (its top, at least 0.05). */
bool cw_foliage_apply_mesh(t3_material *m, t3_geometry *g, const cw_foliage_desc *d);
/* No foliage any more (no-op when m has none); alpha_to_coverage goes back to what it was. */
void cw_foliage_remove(t3_material *m);
/* m has foliage; its values into *out when out is not NULL. */
bool cw_foliage_get(const t3_material *m, cw_foliage_desc *out);
/* Every mesh material under root (each once), the height from each mesh's
 * geometry when d->height is 0; returns how many took it. */
int cw_foliage_apply_tree(t3_object *root, const cw_foliage_desc *d);
/* How many materials have foliage. */
int cw_foliage_count(void);

/* Where the wind puts a point of a plant: the lean (world metres, x y z) a
 * vertex at world (x, z), `share` of the way up (0 root .. 1 at height), on
 * a mesh of height h at instance scale s gets at time t (seconds of the
 * foliage clock, cw_foliage_time). The CPU copy of what the shaders do, for
 * gameplay (a pickup in the grass bobbing with it) and tests. */
t3_vec3 cw_wind_lean(const cw_wind *w, float x, float z, float share, float h, float s, float t);
/* The foliage clock: seconds the shaders' wind is at this frame. */
float cw_foliage_time(void);

#ifdef __cplusplus
}
#endif
#endif

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
 * Movers (bend): plants bend around characters, vehicles and anything else
 * that walks through them. A game registers each mover (cw_foliage_mover_add)
 * and moves it every tick (cw_foliage_mover_update with its world x, z); a
 * plant whose root (an InstancedMesh's instance origin, else its mesh's) is
 * within the mover's radius is pushed away from its centre, fully inside half
 * the radius and easing to nothing at the radius, and turns over by up to
 * `bend` radians at its top (the stem curves: the lean grows with the height
 * share). Behind a mover its trail keeps the plants down and lets them stand
 * up again over the mover's `recover` seconds (a smooth ease to nothing, on
 * the foliage clock); a mover removed leaves its last place to recover too.
 * Pushes of several movers add as vectors, capped at a full push. Each frame
 * the engine packs the movers where they stand, then the trail points that
 * still weigh most, into CW_FOLIAGE_MOVER_SLOTS discs the shaders read (more
 * movers than that: the first ones registered); a trail disc enters and
 * leaves the set at weight 0, so no plant jumps. The bend is on the vertices,
 * so the caster programs bend the shadows too and TAA, the fades and
 * smooth_alpha work as with the wind; like the wind, the velocity pass does
 * not see it. A material with bend 0 ignores movers (its programs skip the
 * work); movers cost nothing when none is near (a branch a vertex).
 *
 * Shadow at the root (shadow_at_root): the sun's (and clustered lights')
 * shadow is taken at one point per instance, its root lifted a quarter of
 * the height, instead of at each fragment. A blade is thinner than a pixel at
 * a distance, so under TAA each blade sampling the shadow map at its own
 * height and position flips between lit and shadowed with the jitter: a
 * stippled, speckled shadow edge through the grass. With one sample per
 * tuft a tuft is wholly lit or wholly shadowed and the edge through the grass
 * reads as the ground's. shadow_soften (0..1) moves each fragment's sample
 * from the root toward its own point by soften times its height share: the
 * upper part of a tuft crossing an edge shades gradually (0: one sample per
 * instance).
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
  float bend;               /* radians the top turns over at a mover's full push (0: movers ignored; 1.1 lays grass well
                             * over; at most 1.5) */
  bool shadow_at_root;      /* one shadow sample per instance, at its root (no speckled shadow edges in grass) */
  float shadow_soften;      /* 0..1: with shadow_at_root, each fragment's sample moves toward its own point by this times
                             * its height share (0: one sample per instance) */
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

/* ── movers ── */
#define CW_FOLIAGE_MOVER_SLOTS 16 /* discs the shaders read a frame (movers, then their trails) */
typedef struct cw_foliage_mover {
  float x, z;       /* world position, metres */
  float radius;     /* metres around it the plants bend (0.5) */
  float strength;   /* 0..1: its push at its centre (1: the material's full bend) */
  float recover;    /* seconds the plants it left take to stand up again (1.5; 0: no trail) */
} cw_foliage_mover;
/* radius 0.5, strength 1, recover 1.5, at (0, 0). */
cw_foliage_mover cw_foliage_mover_default(void);
/* Registers a mover (m NULL: cw_foliage_mover_default); returns its id, -1 when 64 are registered. */
int cw_foliage_mover_add(const cw_foliage_mover *m);
/* Moves mover id to world (x, z): call it every tick it moves. It leaves a trail point every three quarters of its
 * radius. */
void cw_foliage_mover_update(int id, float x, float z);
/* Changes every value of mover id (a jump to m's place leaves no trail between). */
void cw_foliage_mover_set(int id, const cw_foliage_mover *m);
/* Mover id's values into *out (out may be NULL); false for an id not registered. */
bool cw_foliage_mover_get(int id, cw_foliage_mover *out);
/* Unregisters mover id; the plants where it stood stand up again over its recover. */
void cw_foliage_mover_remove(int id);
/* How many movers are registered. */
int cw_foliage_mover_count(void);
/* Forgets every mover and trail at once (a level change). */
void cw_foliage_movers_clear(void);
/* The discs the shaders get this frame, x, z, radius, weight quadruples into out (up to cap); returns how many. */
int cw_foliage_mover_slots(float *out, int cap);
/* The CPU copy of the push a plant rooted at world (x, z) gets this frame: 0..1 (times the material's bend is the
 * angle its top turns over), the direction it is pushed into *dir_x, *dir_z (either may be NULL). */
float cw_foliage_push_at(float x, float z, float *dir_x, float *dir_z);

#ifdef __cplusplus
}
#endif
#endif

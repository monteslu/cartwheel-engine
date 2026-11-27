/* cartwheel/waterline.h: where an object meets the water (a shading part,
 * cartwheel/shading.h): a rider's legs, a hull, a wading figure, a pier's legs.
 *
 * The part shades the object itself, against the water's surface as it is at
 * the object (the waves' height and tilt, cw_water_sample_at):
 *
 *   wet band     everything under a high-water mark is wet: darker by the
 *                surface's porosity (albedo x mix(1, 0.2, porosity), Lagarde's
 *                wet-surface model: fabric darkens most, paint and rubber
 *                hardly) and, for a surface that can shine, glossier with
 *                beaded drops. Cloth stays matte (shine 0). The mark follows the
 *                water up at once and falls back at `fall_rate` (2 cm/s) once
 *                the water drops, and its top dries first, unevenly. A surface
 *                that wicks (porosity) carries the band a few centimetres over
 *                the line even in still water.
 *   soak         a wetness over the whole surface (spray, a fall in the
 *                water: cw_waterline_soak) that dries over `dry_time`.
 *   meniscus     a thin line of sky light just over the contact, at least 1.5
 *                pixels tall, where the water curves up to the object.
 *   caustics     the light the waves gather into moving lines, on the part
 *                under the surface, fading with depth by the water's clarity.
 *                The water draws the submerged part through its refraction,
 *                so they show through the surface; the absorption and murk of
 *                the water on the way are the water's (Water3D see_through,
 *                clarity), never the object's.
 *
 * Use: one cw_waterline per object that moves on its own (a craft, a rider, a
 * wader), the part on every material of that object, and one update per
 * fixed tick with the object's position:
 *
 *   cw_waterline *wl = cw_waterline_new(NULL);
 *   cw_waterline_apply_tree(wl, rider_root, &(cw_waterline_look){ .porosity = 0.8f });   // a suit
 *   ...fixed_update:
 *   cw_waterline_update(wl, water, rider_origin, 0.5f, dt);
 *   if (spray_hit) cw_waterline_soak(wl, 0.3f);
 *
 * The values are per material, so two objects must not share a material
 * (t3_material_clone one per object first; glTF instances share their file's
 * materials). A material of an object out of the water (a craft in the air,
 * a wader on the beach) keeps its band and soak until they dry, and draws as
 * a plain material once they have.
 *
 * Works on MeshStandardMaterial and MeshPhysicalMaterial, skinned or not,
 * with every other part (clustered lights, the cascaded sun, rim, skin
 * shading, a game's surface named before "waterline", GI). Shadow casters are
 * untouched. Cost: a few dozen ALU operations and two caustic texture reads
 * per pixel of the material; nothing for materials without it.
 *
 * Program tables: a material with the part draws the composed state with
 * "waterline" in its extension (xwaterline, xcluster_csm_waterline_gi, ...):
 * tools/programs.mjs finds and adds those like any other state. */
#ifndef CW_WATERLINE_H
#define CW_WATERLINE_H

#include "base.h"
#include "scene.h"
#include "three.h"

#ifdef __cplusplus
extern "C" {
#endif

/* What one material of the object is made of. */
typedef struct cw_waterline_look {
  float porosity; /* 0 sealed (paint, gelcoat, rubber) .. 1 absorbent (fabric): how much darker it gets wet */
  float shine;    /* 0 stays matte when wet (cloth, the default) .. 1 a glossy wet film with beaded drops (paint 0.8, skin 0.4) */
} cw_waterline_look;

/* How the object's waterline behaves. */
typedef struct cw_waterline_desc {
  float wick;              /* metres the band stands over the line in still water, scaled by porosity (0.12) */
  float fall_rate;         /* metres a second the high-water mark falls once the water dropped (0.02) */
  float dry_time;          /* seconds a full soak takes to dry (9) */
  float meniscus;          /* metres of the bright contact line (0.008; never under 1.5 pixels) */
  float meniscus_strength; /* its brightness, emissive units over `sky` (0.25) */
  t3_color sky;            /* the light the meniscus reflects (linear; 0.55, 0.68, 0.82) */
  float wobble;            /* metres the line ripples along the object (0.006) */
  float caustics;          /* strength of the caustics on the submerged part, times the Water3D's `caustics` (1) */
} cw_waterline_desc;

typedef struct cw_waterline cw_waterline;

/* the defaults above */
cw_waterline_desc cw_waterline_default(void);
/* A new waterline (d NULL: the defaults), dry and out of the water. */
cw_waterline *cw_waterline_new(const cw_waterline_desc *d);
/* Takes the part off every material it is on and frees it (NULL: no-op). */
void cw_waterline_free(cw_waterline *w);
/* Replace the values (the state, mark and soak, stays). */
void cw_waterline_set_desc(cw_waterline *w, const cw_waterline_desc *d);

/* Put the part on material m for object w, made of `look` (NULL: { 0.5, 0 }).
 * m must be a MeshStandardMaterial or MeshPhysicalMaterial; false otherwise,
 * or when m already carries CW_SHADING_MAX_PARTS parts. Applying again moves
 * m to w with the new look. The material is retained until removed. */
bool cw_waterline_apply(cw_waterline *w, t3_material *m, const cw_waterline_look *look);
/* The same for every material of every mesh under root (each once); returns how many. */
int cw_waterline_apply_tree(cw_waterline *w, t3_object *root, const cw_waterline_look *look);
/* m loses the part (no-op when it has none). */
void cw_waterline_remove(t3_material *m);
/* Every material under root loses it (call before releasing a model whose materials have it). Returns how many. */
int cw_waterline_remove_tree(t3_object *root);
/* m has the part; its object and look into *w and *look when not NULL. */
bool cw_waterline_get(const t3_material *m, cw_waterline **w, cw_waterline_look *look);
/* Materials with the part now. */
int cw_waterline_count(void);

/* Once per fixed tick: the water at the object. `origin` is a point the
 * object carries (its node's world position): the mark is kept relative to
 * it, so a band stays where it is on the object when the object leaves the
 * water (a craft in the air). The surface is fitted through the water's
 * height at origin and at four points `radius` metres around it (0: the
 * height and normal at origin alone); take about half the object's length.
 * A null or non-Water3D `water` takes the object out of the water. dt: the
 * tick's seconds. Deterministic (cw_water_sample_at). */
void cw_waterline_update(cw_waterline *w, cw_node water, t3_vec3 origin, float radius, float dt);
/* The surface given directly instead (a game's own water): world height of
 * the surface at origin and its unit normal. */
void cw_waterline_update_plane(cw_waterline *w, t3_vec3 origin, float height, t3_vec3 normal, float dt);
/* Wet the whole surface by `amount` (0..1, adds up to 1: a wave over the
 * deck, spray, 1 for a fall in the water). */
void cw_waterline_soak(cw_waterline *w, float amount);
/* Throw water `height` metres up the object over the line now (a slap of a
 * wave, a splash): the mark rises to it and falls back as usual. */
void cw_waterline_splash(cw_waterline *w, float height);

/* What a waterline is doing (for a HUD, a sound, a test). */
typedef struct cw_waterline_state {
  bool in_water;  /* the surface is known and reaches within `radius` + 2 m of origin */
  float level;    /* metres the surface stands over origin (negative: under it) */
  float mark;     /* metres the high-water mark stands over origin */
  float band;     /* metres of wet band over the line now (mark - level, 0 when under) */
  float soak;     /* 0..1 */
  t3_vec3 normal; /* the fitted surface's normal */
} cw_waterline_state;
/* What w is doing now (zeroed for a NULL w). */
void cw_waterline_state_of(const cw_waterline *w, cw_waterline_state *out);

/* The light the submerged parts show (every waterline): the direction toward
 * the sun (world, unit; the default is a high sun), which projects the
 * caustics as the water's floor does. Setting it is a memory write. */
void cw_waterline_set_sun(t3_vec3 to_sun);

#ifdef __cplusplus
}
#endif
#endif

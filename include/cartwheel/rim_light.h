/* cartwheel/rim_light.h: a rim light, a coloured glow along a surface's
 * silhouette edges (a shading part, cartwheel/shading.h).
 *
 * Where a surface turns away from the camera its edge glows in the rim's
 * colour: glow = color x strength x (1 - N.V)^power, added to the
 * material's emissive light (so it shows in shadow and at night, and bloom
 * picks it up). It fades in with distance from the camera: below
 * `near` metres it is `close` of its full strength, past `far` it is all
 * of it, smooth in between. That is what a split-screen shooter wants for
 * telling teams apart: a fighter 30 m away reads as a red or blue outline,
 * one at arm's length is not painted over. Also: a highlight on pickups or
 * interactive objects, a "spawn protected" shimmer (pulse the strength from
 * code), a selection glow in a menu scene.
 *
 * Per material: every mesh drawing the material gets the same rim (glTF
 * instances share their file's materials: clone a material per character,
 * t3_material_clone, for a per-character colour; MeshInstance3D primitives
 * of one look share one material in a tree). Changing the values is free (a
 * uniform per draw); the part is added once.
 *
 * Works on MeshStandardMaterial and MeshPhysicalMaterial, skinned or not,
 * with every other part (clustered lights, the cascaded sun, skin shading,
 * GI). Costs a few ALU operations per pixel of the material; nothing when
 * no material has a rim. Shadow casters are untouched. Every quality preset
 * draws it (it is a readability feature, not decoration).
 *
 * Program tables: a material with a rim draws the composed state with
 * "rim" in its extension (xrim, xcluster_rim_gi, ...): tools/programs.mjs
 * finds and adds those like any other state. */
#ifndef CW_RIM_LIGHT_H
#define CW_RIM_LIGHT_H

#include "base.h"
#include "three.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct cw_rim_light_desc {
  t3_color color;   /* the glow's colour (linear; 1, 0.3, 0.2) */
  float strength;   /* its brightness, emissive units (2) */
  float power;      /* how tight to the edge, 1 = broad, 8 = a thin line (3) */
  float near, far;  /* metres from the camera where it fades in (6 to 20) */
  float close;      /* the share of the strength it keeps below `near` (0.15) */
} cw_rim_light_desc;

/* the defaults above */
cw_rim_light_desc cw_rim_light_default(void);

/* Give material m a rim (d NULL: the defaults). m must be a
 * MeshStandardMaterial or MeshPhysicalMaterial; false otherwise, or when m
 * already carries CW_SHADING_MAX_PARTS parts. Applying again replaces the
 * values (cheap: call it every frame to pulse a rim). The material is
 * retained until cw_rim_light_remove. */
bool cw_rim_light_apply(t3_material *m, const cw_rim_light_desc *d);
/* No rim any more (no-op when m has none). */
void cw_rim_light_remove(t3_material *m);
/* m has a rim; its values into *out when out is not NULL. */
bool cw_rim_light_get(const t3_material *m, cw_rim_light_desc *out);
/* Every material of every mesh under root gets the rim (each material once;
 * clone first for a per-object colour). Returns how many materials. */
int cw_rim_light_apply_tree(t3_object *root, const cw_rim_light_desc *d);
/* Every material under root loses its rim (call before releasing a model
 * whose materials have one: the rim keeps its materials alive). Returns how
 * many materials. */
int cw_rim_light_remove_tree(t3_object *root);
/* Materials with a rim now. */
int cw_rim_light_count(void);

#ifdef __cplusplus
}
#endif
#endif

/* cartwheel/toon.h: toon (cel) shading, three.js r187's MeshToonMaterial.
 *
 * A toon surface is lit in flat tones instead of a smooth falloff: the light
 * a surface gets is read from a gradient map, a ramp with one tone a texel
 * (its left end faces away from the light), so a ramp of three texels paints
 * shadow, mid tone and light with hard edges between them. With no ramp the
 * material has three.js's own two tones (0.7 and 1). The material keeps its
 * colour and colour map, emissive colour, map and intensity (a glow that
 * ignores the light: fuses, eyes, signs), normal map, ambient occlusion,
 * light map, alpha map and cutout, vertex colours, transparency and sides;
 * it has no roughness, metalness, reflections or highlights.
 *
 * Every light lights it: ambient and hemisphere light, three.c's lights,
 * the clustered omni and spot lights and the cascaded sun, each with its
 * shadows (cartwheel/lighting.h). Each light is banded on its own, so two
 * lights give two sets of bands. The plain lighting model of the Very Low
 * preset, rim lights and skin shading are for the physically based
 * materials and leave a toon one as it is.
 *
 * From a scene: MeshInstance3D `shading = TOON` and `toon_bands` (the
 * node's primitive, or every material of its mesh resource or glTF scene).
 * From code: cw_toon_material for one material, cw_toon_apply_tree for a
 * loaded model.
 *
 * Cost: cheaper than the physically based materials (no specular, no
 * environment lookup); one texture read per light. Program tables: a toon
 * material draws states of kind "toon" (toon+gmap+...): tools/programs.mjs
 * finds and adds them like any other state. */
#ifndef CW_TOON_H
#define CW_TOON_H

#include "base.h"
#include "three.h"

#ifdef __cplusplus
extern "C" {
#endif

enum { CW_TOON_MAX_BANDS = 16 };

/* The shared ramp of `bands` equal tones (2 to CW_TOON_MAX_BANDS: texel i of
 * n is (i + 1) / n of the light, nearest filtered as three.js asks of a
 * gradient map). Borrowed: retain it to keep it. NULL for fewer than 2
 * bands (three.js's two tones need no map). */
t3_texture *cw_toon_ramp(int bands);

/* A new MeshToonMaterial with src's surface (the list above) and the ramp
 * of `bands` tones (below 2: three.js's two tones). src is left as it is;
 * its shading parts are not carried over. The caller owns the result. For a
 * ramp of your own (uneven steps, a tinted shadow tone is not possible: the
 * map's red channel scales the light), set gradient_map on the result with
 * t3_material_set_texture(m, T3_GRADIENT_MAP, tex): a DataTexture with both
 * filters T3_NEAREST. */
t3_material *cw_toon_material(const t3_material *src, int bands);

/* Every lit material (Lambert, Phong, Standard, Physical) of every mesh
 * under root is replaced by its toon counterpart. Meshes that shared a
 * material share its toon copy, across calls too (two instances of one
 * glTF, each given to this, still draw with the same materials). Returns
 * how many material slots changed. */
int cw_toon_apply_tree(t3_object *root, int bands);
/* The reverse: every toon material cw_toon_apply_tree put under root gives
 * way to the material it was made from. Returns how many slots changed. */
int cw_toon_restore_tree(t3_object *root);
/* m is a toon material made by cw_toon_apply_tree (and still drawn). */
bool cw_toon_is_applied(const t3_material *m);

#ifdef __cplusplus
}
#endif
#endif

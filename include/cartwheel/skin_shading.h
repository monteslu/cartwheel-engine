/* cartwheel/skin_shading.h: skin shading, a cheap subsurface look for
 * characters (a shading part, cartwheel/shading.h).
 *
 * Real skin scatters light under its surface: the shadow side of a face goes
 * soft and warm instead of dark grey, and thin parts (ears, nostrils, finger
 * webs) glow when lit from behind. Plain PBR shades skin like painted
 * plastic. This part adds two cheap terms to the material's own lighting
 * (MeshStandardMaterial or MeshPhysicalMaterial, so sheen, clear coat and
 * the rest still apply), for every light that reaches it (the scene's own
 * lights, clustered lights, the cascaded sun):
 *
 *   - wrap: diffuse light reaches past the line where the surface turns away
 *     from the light, each colour channel by `wrap` times its `scatter`
 *     component, so red bleeds furthest (a warm, soft terminator). The lit
 *     side is unchanged; the extra light is (N.L + w) / (1 + w) - N.L.
 *   - translucency: light from behind the surface, seen through it: strongest
 *     looking toward a light past a thin edge. r186's MeshSSSNodeMaterial
 *     term (fast translucency): pow(saturate(V . -(L + N * distortion)),
 *     power) + ambient, times `translucency`, the scatter colour, the
 *     surface's diffuse colour and the light's colour.
 *
 * No screen-space pass, no extra render targets, no textures: a few ALU
 * operations per light per skin pixel. A light's shadow darkens both terms
 * as it darkens the rest of the light, so translucency needs the light not
 * to be blocked by the body itself (a rim light behind a head, say).
 *
 * Quality: on Low (cw_quality_scale("skin") is 0) every skin material draws
 * as its plain PBR material (the part is removed, so Low pays nothing); the
 * other presets draw it. The engine switches when the setting changes.
 *
 * From glTF: a material loaded through the resource system (CW_RES_GLTF)
 * gets skin shading when the file asks for it:
 *   - KHR_materials_diffuse_transmission: translucency = its
 *     diffuseTransmissionFactor, scatter = its diffuseTransmissionColorFactor
 *     (the rest as cw_skin_shading_default);
 *   - extras "skin_shading": true (the defaults) or an object with any of
 *     "scatter" [r, g, b], "wrap", "translucency", "distortion", "power",
 *     "ambient" (over the defaults, or over what the extension gave).
 * Or by material name after loading: cw_skin_shading_apply_named.
 *
 * Cost: see docs/guides/rendering-3d.md, "Characters". */
#ifndef CW_SKIN_SHADING_H
#define CW_SKIN_SHADING_H

#include "base.h"
#include "three.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct cw_skin_shading_desc {
  t3_color scatter;    /* colour that scatters furthest under the skin (linear; 1, 0.35, 0.22) */
  float wrap;          /* how far light wraps past the terminator, 0 to 1 (0.55) */
  float translucency;  /* strength of light seen through thin parts from behind, 0 = off (0.35) */
  float distortion;    /* how much the surface normal bends the light seen through (0.25) */
  float power;         /* how tightly the glow gathers toward the light (3) */
  float ambient;       /* translucency that reaches every lit pixel, a softening term (0) */
} cw_skin_shading_desc;

/* the defaults above (a light-skinned human; darker skin wants a deeper
 * scatter, e.g. 0.9, 0.3, 0.2, and less translucency) */
cw_skin_shading_desc cw_skin_shading_default(void);

/* Shade material m as skin (d NULL: the defaults). m must be a
 * MeshStandardMaterial or MeshPhysicalMaterial; false otherwise, or when m
 * already carries CW_SHADING_MAX_PARTS parts. Applying again replaces the
 * values. The material is retained until cw_skin_shading_remove. Every mesh
 * drawing m is affected (glTF instances share their file's materials: clone
 * a material for a per-character tint). */
bool cw_skin_shading_apply(t3_material *m, const cw_skin_shading_desc *d);
/* Back to plain PBR (no-op when m is not skin). */
void cw_skin_shading_remove(t3_material *m);
/* m is skin; its values into *out when out is not NULL. */
bool cw_skin_shading_get(const t3_material *m, cw_skin_shading_desc *out);
/* Every material named `material_name` (t3_material.name: the glTF
 * material's name) on the meshes under root gets skin shading. Returns how
 * many materials (each counted once). */
int cw_skin_shading_apply_named(t3_object *root, const char *material_name, const cw_skin_shading_desc *d);
/* Materials shaded as skin now. */
int cw_skin_shading_count(void);
/* The quality setting draws skin shading (false on Low: skin materials draw
 * as plain PBR). */
bool cw_skin_shading_enabled(void);

#ifdef __cplusplus
}
#endif
#endif

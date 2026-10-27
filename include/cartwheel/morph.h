/* cartwheel/morph.h: morph targets (blend shapes) by name.
 *
 * A glTF mesh's morph targets load with it (positions and normals, on plain
 * and skinned meshes, GL and WebGPU); their names come from the mesh's
 * extras.targetNames (what Blender and most exporters write), and
 * t3_mesh.morph_influences holds each target's weight. These calls set and
 * read weights by name over a whole model (every mesh under a root that has
 * a target of that name: a face split into several primitives moves as
 * one), so game code drives expressions without knowing the mesh layout:
 *
 *   cw_morph_set(fighter, "smile", 0.8f);
 *   cw_morph_set(fighter, "blink", blink);
 *
 * Animation clips drive the same weights: a glTF animation's "weights"
 * channel becomes one float track per target in cw_anim_from_t3_clip
 * ("model@Mesh:morphTargetInfluences[i]"), and a hand-made track may name
 * the target instead of its index ("model@Mesh:morphTargetInfluences[smile]"),
 * so an AnimationPlayer or AnimationTree blends faces with the body.
 * Code and clips write the same weights: whichever runs last in the frame
 * wins.
 *
 * Instances of one glTF (cw_gltf_instantiate) share geometry, so the morph
 * target data (one texture per geometry on the GPU) is stored once; each
 * instance has its own weights.
 *
 * Cost: a morphed mesh is drawn on its own (never auto-instanced) and its
 * vertex shader reads every target's offset per vertex: about one texture
 * fetch per target per vertex (two with normals). Keep face targets on the
 * head mesh, not the whole body. */
#ifndef CW_MORPH_H
#define CW_MORPH_H

#include "base.h"
#include "three.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Set target `name`'s weight (0 to 1 usually; any value is allowed) on
 * every mesh under root (root included) that has it. Returns how many meshes
 * changed; 0 when no mesh has a target of that name. */
int cw_morph_set(t3_object *root, const char *name, float weight);
/* The weight of target `name` on the first mesh under root that has it
 * (depth first), 0 when none does. */
float cw_morph_get(const t3_object *root, const char *name);
/* Every weight of every morphed mesh under root to 0. Returns the meshes. */
int cw_morph_reset(t3_object *root);
/* The distinct target names under root (up to 256), in mesh order (out may
 * be NULL to count); returns the count (which may exceed cap). Names are owned by the
 * glTF (keep it loaded). */
int cw_morph_names(const t3_object *root, const char **out, int cap);

#ifdef __cplusplus
}
#endif
#endif

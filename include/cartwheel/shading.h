/* cartwheel/shading.h: shading parts, three.c generator extensions that
 * stack on one material.
 *
 * three.c gives a material one generator extension (t3_material_set_extension,
 * state x<name>): r186 TSL captured into a program, filled at run time by a C
 * hook. Several engine features change how every surface shades (terrain or
 * water decide the surface, clustered lights add direct light, the GI module
 * adds reflections and bounce light), so one material often needs more than
 * one. A part is one such extension; a material carries a list of parts, and
 * its extension is the composed name: the parts in the fixed order below,
 * joined by '_' ("terrain_gi", "cluster_gi"). Nothing is written by hand per
 * combination:
 *
 *   - the generator side: a part is a module src/<module>/extensions/<part>.mjs
 *     (export name, apply()), as any extension. tools/programs.mjs writes, for
 *     every composed state a manifest names (standard+senv+xcluster_gi), a
 *     module that applies the parts in that order to the capture's material,
 *     and hands it to three.c's generator. Surface parts set colorNode,
 *     normalNode, positionNode; lighting parts add an r186 LightingNode to the
 *     material's lights; the GI part sets envNode (reflections) and adds a
 *     LightingNode (the probe grid's irradiance). So parts compose without
 *     knowing each other, as long as each names its uniforms and textures
 *     "ext.<unique name>" and its shared uniform groups uniquely.
 *   - the run-time side: each part registers its fill hook on a renderer
 *     (cw_shading_part); a composed extension's hook calls every part's hook
 *     in order on the same draw.
 *
 * Order (fixed, global): surface parts (terrain, terrainlow, terrainfar,
 * water), then lighting (cluster), then indirect light (gi); any other part
 * between lighting and indirect, by name.
 *
 * Cost: none per part beyond its own hook. three.c does not auto-instance a
 * mesh whose material carries an extension (each such mesh is its own draw)
 * unless every part of it says its hook reads nothing of the object
 * (cw_shading_part_batching): then meshes sharing geometry and material are
 * one instanced draw, as plain ones are. */
#ifndef CW_SHADING_H
#define CW_SHADING_H

#include "base.h"
#include "three.h"

#ifdef __cplusplus
extern "C" {
#endif

enum { CW_SHADING_MAX_PARTS = 4 };

/* The renderer's hook for part `name` ([a-z0-9], up to 15 characters): it
 * fills that part's "ext." values for each draw of any material carrying the
 * part, alone or composed. fill NULL removes it. Also installs every composed
 * extension that includes the part. */
void cw_shading_part(t3_renderer *r, const char *name, t3_extension_fill fill, void *user);
/* Part `name`'s hook on r reads nothing of the object it fills for (only
 * per-frame values, or ones a batch keeps equal: receive_shadow, render
 * order), so meshes whose material carries it may be auto-instanced. A
 * composed extension batches when all of its parts do. Off by default. */
void cw_shading_part_batching(t3_renderer *r, const char *name, bool on);
/* A part named `name` was registered (cw_shading_part) on some renderer: the
 * engine's lighting composes its own parts (cluster, csm) with it. */
bool cw_shading_part_known(const char *name);
/* Forget a renderer (before t3_renderer_destroy). */
void cw_shading_forget(t3_renderer *r);

/* Add part `name` to material m (no-op when it has it): m's extension becomes
 * the composed name. false when m would carry more than CW_SHADING_MAX_PARTS
 * parts or the composed name would pass 31 characters. */
bool cw_shading_add(t3_material *m, const char *name);
/* Remove part `name` from m (no-op when it lacks it). */
void cw_shading_remove(t3_material *m, const char *name);
/* m carries part `name`. */
bool cw_shading_has(const t3_material *m, const char *name);
/* Replace m's parts by the composed name of `parts` (n of them, any order):
 * what cw_shading_add gives, from scratch. */
bool cw_shading_set(t3_material *m, const char *const *parts, int n);

#ifdef __cplusplus
}
#endif
#endif

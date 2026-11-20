/* cartwheel/surface.h: a surface of the game's own that keeps the engine's
 * lighting.
 *
 * A game writes how one kind of surface looks (a dissolve, triplanar rock,
 * grass that sways, a force field, lava) as r187 TSL in a module of its own,
 * shaders/<name>.mjs: the module names its parameters and returns the
 * surface's inputs (colour, normal, roughness, metalness, emissive, opacity,
 * a cut-out mask, vertex positions) and, if it wants, a last word on the lit
 * colour. Everything that lights a surface stays the engine's: three.c's
 * lights and shadows, the clustered lamps, the sun's cascades, reflection
 * probes and the light probe grid, screen-space reflections, fog, decals.
 * A material with a surface is shaded by all of them exactly as a plain
 * MeshStandardMaterial is; with its effect at zero it draws the same pixels.
 *
 *   // once, at start (before or after the scene exists)
 *   static const cw_surface_param dissolve_params[] = {
 *     { "amount", CW_SURFACE_FLOAT },
 *     { "edge", CW_SURFACE_VEC4, .value = { 6, 1.6f, 0.2f, 0.06f } },
 *   };
 *   cw_surface_register(&(cw_surface_desc){ .name = "dissolve", .params = dissolve_params, .param_count = 2,
 *                                           .node_type = "DissolveSurface3D" });
 *   // on a material
 *   cw_surface_apply(material, "dissolve");
 *   cw_surface_set_float(material, "dissolve", "amount", 0.4f);
 *
 * The module's parameter list and this one must agree in names and types
 * (docs/guides/rendering-3d.md, "Writing your own surface", has both sides
 * and two worked examples). A surface is a shading part (cartwheel/shading.h)
 * with the trait CW_PART_SURFACE: a material's program is the composed state
 * (standard+l0001+xcluster_csm_dissolve_gi), captured offline into the app's
 * program table by tools/programs.mjs like any other state. Name the module in
 * the table's manifest ("# extend: ../shaders/dissolve.mjs").
 *
 * Values. Each parameter has the registered default, a value for every
 * material of the surface (cw_surface_set_global: time, wind), and a value
 * per material (cw_surface_set...), which wins. Setting one is a memory
 * write; the values travel with each draw. A texture may be replaced at any
 * time, and its pixels changed in place (write t->pixels, then t->version++).
 *
 * Nodes. With `node_type`, the surface is also a node type whose properties
 * are the parameters: put one under a MeshInstance3D (or any Node3D) in a
 * scene file and that node's meshes get the surface on materials of their
 * own, driven by the node's properties, which an AnimationPlayer can key
 * like any other.
 *
 * Draws. Meshes that share geometry and a material with a surface are one
 * instanced draw when every part on the material allows it: a surface does;
 * the clustered lights' and cascades' parts do when LightingSettings.batching
 * is on; the gi part (a scene with probes) never does, since each mesh picks
 * its own probes. */
#ifndef CW_SURFACE_H
#define CW_SURFACE_H

#include "base.h"
#include "three.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum cw_surface_type {
  CW_SURFACE_FLOAT,   /* 'float' */
  CW_SURFACE_VEC2,    /* 'vec2' */
  CW_SURFACE_VEC3,    /* 'vec3' */
  CW_SURFACE_VEC4,    /* 'vec4'; with count > 1 an array, { type: 'vec4', count: N } */
  CW_SURFACE_COLOR,   /* 'color': linear rgba, a vec4 in the module */
  CW_SURFACE_MAT4,    /* 'mat4', column-major */
  CW_SURFACE_TEXTURE, /* { texture: ... } */
} cw_surface_type;

typedef struct cw_surface_param {
  const char *name;     /* [a-zA-Z0-9]: the module's parameter name */
  cw_surface_type type;
  int count;            /* VEC4 only: elements of an array (0 or 1: one value), at most 32 */
  float value[4];       /* the default (an array's every element; a MAT4's is the identity, a TEXTURE's white) */
} cw_surface_param;

typedef struct cw_surface_desc {
  const char *name;               /* the module's name ([a-z0-9], at most 10 characters: it is part of every state's name) */
  const cw_surface_param *params; /* copied */
  int param_count;                /* at most 32 */
  bool moves_vertices;            /* the module returns `position`: no mesh decal is clipped from its meshes */
  const char *node_type;          /* NULL, or the node type to register for it ("DissolveSurface3D") */
} cw_surface_desc;

/* Register a surface (once; again with the same name replaces the defaults).
 * false, with the reason logged, for a bad name or parameter list. */
bool cw_surface_register(const cw_surface_desc *d);
/* A surface of that name is registered. */
bool cw_surface_known(const char *surface);

/* Put the surface on m (a MeshStandardMaterial, MeshPhysicalMaterial,
 * MeshLambertMaterial or MeshPhongMaterial); a no-op when it is on it. The
 * material is retained until cw_surface_remove. */
bool cw_surface_apply(t3_material *m, const char *surface);
/* Take the surface off m and drop its values (a no-op when m lacks it). */
void cw_surface_remove(t3_material *m, const char *surface);
/* m carries the surface (through cw_surface_apply or a surface node). */
bool cw_surface_has(const t3_material *m, const char *surface);
/* The same for every material under root (each once); returns how many took it. */
int cw_surface_apply_tree(t3_object *root, const char *surface);

/* A parameter of m's surface from n floats: 1 to 4 for FLOAT to COLOR, 16
 * for a MAT4, 4 x count for an array. false (logged once) for a material
 * without the surface, an unknown parameter or a wrong n. */
bool cw_surface_set(t3_material *m, const char *surface, const char *param, const float *v, int n);
/* The same for a FLOAT parameter. */
bool cw_surface_set_float(t3_material *m, const char *surface, const char *param, float v);
/* One element of a VEC4 array. */
bool cw_surface_set_at(t3_material *m, const char *surface, const char *param, int index, const float v[4]);
/* A TEXTURE parameter (retained; NULL goes back to the surface's value). */
bool cw_surface_set_texture(t3_material *m, const char *surface, const char *param, t3_texture *t);
/* What a draw of m gets for the parameter (its own value, else the surface's,
 * else the default): up to cap floats into out, the count returned (0: unknown). */
int cw_surface_get(const t3_material *m, const char *surface, const char *param, float *out, int cap);
/* Back to the surface's value. */
void cw_surface_unset(t3_material *m, const char *surface, const char *param);

/* The value every material of the surface draws with unless it has its own. */
bool cw_surface_set_global(const char *surface, const char *param, const float *v, int n);
/* The texture every material of the surface samples unless it has its own (retained; NULL: white). */
bool cw_surface_set_global_texture(const char *surface, const char *param, t3_texture *t);

/* How many materials carry the surface now. */
int cw_surface_material_count(const char *surface);

#ifdef __cplusplus
}
#endif
#endif

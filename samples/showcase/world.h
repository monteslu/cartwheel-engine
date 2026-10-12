/* showcase world generation: the valley terrain (chunked, three LOD levels
 * each, skirts against cracks), the forest (instanced), the village (houses,
 * lamp posts, the square) and the lights that live in it. Everything comes
 * from cw_ math and a seeded cw_rng, so every host builds the same world. */
#ifndef SHOWCASE_WORLD_H
#define SHOWCASE_WORLD_H

#include "cartwheel/scene.h"

#define SC_TERRAIN_SIZE 480.0f  /* metres, centred on the origin */
#define SC_CHUNKS 12            /* per side */
#define SC_LODS 3
#define SC_LAMPS 128
#define SC_FIREFLIES 64
#define SC_WINDOWS 96
#define SC_TREES 1400
/* real lights: three.c lights a surface with at most 8 point lights, so the
 * campfire and the SC_LAMP_LIGHTS lit lamps nearest the view get one; every
 * other lamp (and every firefly) shows by its emissive glow alone */
#define SC_LAMP_LIGHTS 7

/* terrain height (metres) at x, z: deterministic */
float sc_height(float x, float z);

typedef struct sc_lamp { t3_vec3 pos; float stagger; float on; } sc_lamp;
typedef struct sc_firefly { float cx, cz, r, speed, phase, h; } sc_firefly;
/* one of the lamps' real lights: the lamp it lights (-1 none) and how far it
 * has faded in (a light moving to another lamp fades out first) */
typedef struct sc_lamp_light { cw_node light; int lamp; float fade; } sc_lamp_light;

typedef struct sc_world {
  t3_lod *chunks[SC_CHUNKS * SC_CHUNKS];
  t3_instanced_mesh *bulbs, *windows, *fly_glow;
  sc_lamp lamps[SC_LAMPS];
  sc_firefly flies[SC_FIREFLIES];
  sc_lamp_light lamp_lights[SC_LAMP_LIGHTS];
  float window_stagger[SC_WINDOWS];
  t3_color window_color[SC_WINDOWS];
  cw_node campfire;
  cw_node fire, sparks, fire_smoke;      /* GPUParticles3D on the campfire */
  t3_vec3 chimney_top[16];
  cw_node smokes[8]; int smoke_count;    /* unlit: dimmed with the daylight */
  float smoke_light;
  bool fire_on;
  t3_vec3 campfire_pos;
  int tree_count, triangles_l0;
  bool ok;                  /* every buffer passed validation */
} sc_world;

/* Build everything under `root` (a Node3D). Returns false (logged) when a
 * generated buffer fails validation: nothing invalid is ever handed to GL. */
bool sc_world_build(sc_world *w, cw_node root);

/* per tick: lamps fade toward on/off by the sun's elevation (degrees), the
 * fireflies fly, the campfire flickers, windows light up */
void sc_world_tick(sc_world *w, uint64_t tick, float sun_elevation);
/* per tick, after the camera moves: the lamps' real lights go to the lit
 * lamps nearest `focus` (a point ahead of the camera) */
void sc_world_lamp_lights(sc_world *w, t3_vec3 focus);

/* the effects: GPU particles (the campfire's flames, sparks and
 * smoke, chimney smoke), decals (the fire's scorch, wear on the road) and a
 * VisibilityManager (frustum and occlusion culling, primitive LOD) for the
 * village's mesh nodes */
void sc_world_build_fx(sc_world *w, cw_node root);

/* chunks drawn at each LOD level last frame (after the render) */
void sc_world_lod_counts(const sc_world *w, int counts[SC_LODS]);
int sc_world_lamps_on(const sc_world *w);

#endif

/* cartwheel/splats.h: blob shadows and light splats, the cheap stand-ins
 * for dynamic shadows and dynamic lights on ground that draws baked
 * (LightmapGI shading BAKED, cartwheel/lightmap.h, takes no real-time light
 * or shadow).
 *
 * A splat set is one instanced draw of flat quads lying on the ground, each
 * placed, stretched and turned by the caller every frame:
 *
 *   CW_SPLATS_SHADOW  a soft dark blob under a moving object: the colour
 *                     below is multiplied by 1 - strength x falloff (blend
 *                     ZERO, ONE_MINUS_SRC_COLOR), so a strength of 0.6
 *                     takes 60% of the light at the centre. Stretch it away
 *                     from the sun for a low sun's long shadow.
 *   CW_SPLATS_LIGHT   a pool of added light on the ground under a moving
 *                     light (a muzzle flash, a fire, a lamp a player
 *                     carries): additive, the colour is the light's colour
 *                     x strength (HDR, so it blooms), the falloff that of a
 *                     point light above the ground.
 *
 * Cost: one draw for the whole set; pixels only where a splat lies (a
 * shadow under 2000 enemies is a few percent of a 1080p frame). Splats test
 * depth and write none, so what stands on them hides them. Render only:
 * nothing here reaches the simulation. Opt in: nothing draws a splat unless
 * a game makes a set. */
#ifndef CW_SPLATS_H
#define CW_SPLATS_H

#include "scene.h"
#include "three.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum { CW_SPLATS_SHADOW, CW_SPLATS_LIGHT } cw_splats_kind;
typedef struct cw_splats cw_splats;

/* A set of up to `capacity` splats drawn under a new Node3D `name`, a
 * child of parent. NULL when capacity < 1. */
cw_splats *cw_splats_new(cw_node parent, const char *name, cw_splats_kind kind, int capacity);
void cw_splats_free(cw_splats *s);
/* Start this frame's splats (none drawn until added). */
void cw_splats_begin(cw_splats *s);
/* One splat centred at `at` (its height: the ground's, a few centimetres are
 * added against z-fighting), `length` along its yaw (radians about +Y, 0 =
 * along +Z) and `width` across, metres. strength: SHADOW 0..1 (the share of
 * the light taken at its centre); LIGHT any (x color). The centre can sit
 * off the middle: `offset` (-1..1) slides it toward the far end, for a long
 * shadow that starts at the caster's feet. false when the set is full. */
bool cw_splats_add(cw_splats *s, t3_vec3 at, float length, float width, float yaw, float offset, t3_color color,
                   float strength);
/* A LIGHT set: a splat under every visible OmniLight3D under root that has
 * energy and is not baked (bake_direct off), sized to where its light on
 * ground at height ground_y falls to a twentieth (at most its range), its
 * strength the light's irradiance straight below (energy / height^2) x
 * scale: give scale the ground's albedo / PI and the pool is about the
 * light a lit ground would show. Returns how many it added. */
int cw_splats_add_lights(cw_splats *s, cw_node root, float ground_y, float scale);
/* Upload this frame's splats (after the last add). */
void cw_splats_end(cw_splats *s);
int cw_splats_count(const cw_splats *s);
/* The set's instanced mesh (render order, layers, visibility). */
t3_instanced_mesh *cw_splats_mesh(cw_splats *s);

#ifdef __cplusplus
}
#endif
#endif

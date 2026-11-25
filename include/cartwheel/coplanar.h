/* Coplanar faces: faces that fight for the depth test, found and layered apart (src/render/coplanar.c).
 *
 * Two faces lying in one plane, facing the same way (or either way, when one is drawn from both sides) and
 * overlapping, one of them writing depth (a tile laid flat on a board, a card on a card, a trim laid flush on a wall,
 * two boxes sharing a face) have no depth order: the depth test picks one or the other pixel by pixel, a little
 * differently every frame the camera moves, so patches of the picture swap between the two surfaces' look
 * (z-fighting). Two that look the same where they overlap (one material, one shading normal, the same texture
 * coordinates and colours there) do not count: whichever wins, the pixel is the same.
 *
 *   cw_coplanar_scan            counts those pairs among the meshes drawn in a 3D scene (world space, every visible
 *                               depth-tested mesh and instance; skinned meshes, lines, points and sprites are left
 *                               out), by region (the scene nodes they hang under)
 *   cw_coplanar_separate        layers them apart where a game builds its still world (a board and the pieces laid on
 *                               it). Faces move as whole patches (the faces of one mesh copy in one plane and one
 *                               material that meet), and the walls meeting a moved patch stretch to it, so nothing
 *                               cracks. Of two patches in one mesh copy the one drawn later comes out in front (it is
 *                               what the depth test mostly showed already); of two in different meshes, the smaller
 *                               (the detail on what it is laid on: the tile on the board, the top card on the one
 *                               under it). A geometry drawn more than once is layered in its own space first; an
 *                               instance lying on another instance of its mesh in an upright plane steps aside.
 *   cw_coplanar_separate_model  the same for a model's parts in its own space, before it is placed
 *   cw_coplanar_init            registers the debug command `coplanar` (the scan of the app's 3D scene, as JSON) and
 *                               the debug fields level.coplanar_pairs / level.coplanar_moved; `coplanar selftest`
 *                               checks the detector on made-up faces (a doubled face must count, neighbours and faces
 *                               5 mm apart must not)
 *
 * It moves vertices of the geometries it is given (a geometry two meshes share is given each its own copy first), so
 * call it once where the still world is built, not every tick: a piece that moves later is laid apart by the game
 * (a stack's height step), or the world is separated again after it settles; cw_coplanar_last_ms says
 * what the last call took.
 *
 * Switches (native development and the gates; read from the environment, or from cw_coplanar_set_options):
 * COPLANAR_CONTROL=1 adds a doubled face to the scene (two squares in one plane, cut into triangles differently),
 * which the scan must find; COPLANAR_SEPARATE=0 leaves the world as built (a sweep's control);
 * COPLANAR_SWEEP="from,x,y,z,tx,ty,tz,dx,dy,dz" holds the game still from frame `from` and glides the camera;
 * COPLANAR_DUMP=n prints the first n pairs of a scan face by face, COPLANAR_DEBUG=1 each separation pass;
 * COPLANAR_CELL_CAP caps the scan's grid cells. The guide: docs/guides/rendering-3d.md#coplanar-faces. */
#ifndef CW_COPLANAR_H
#define CW_COPLANAR_H

#include <stdbool.h>

#include "three.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct cw_coplanar_region {
  char name[96];
  int pairs;
  float width;  /* the widest overlap of its pairs (the narrowest way across one pair), m */
  float at[3];  /* that pair's middle (world) */
} cw_coplanar_region;

typedef struct cw_coplanar_report {
  int meshes, faces, pairs;
  int nregions;
  cw_coplanar_region regions[48]; /* the most pairs first */
} cw_coplanar_report;

/* the pairs among root's drawn meshes (root's matrices as they stand) */
void cw_coplanar_scan(t3_object *root, cw_coplanar_report *out);
/* layer the overlapping same-plane faces of root's plain meshes (not instanced, not skinned) apart by eps (metres; at
 * least twice the plane tolerance). A geometry that several meshes draw is first layered in its own space; two copies
 * of one geometry lying on each other are left as they are (moving the geometry moves both: see
 * cw_coplanar_separate_ex), except instances of one InstancedMesh in an upright plane, which step aside. Returns the
 * vertices moved; *left the pairs among those meshes after (0 when every one was separated). Does nothing (returns 0)
 * under COPLANAR_SEPARATE=0. Call it once the meshes exist (a MeshInstance3D builds its mesh in the first frame's
 * pre_render). */
int cw_coplanar_separate(t3_object *root, float eps, int *left);
#define CW_COPLANAR_UNSHARE 1u
/* cw_coplanar_separate with flags. CW_COPLANAR_UNSHARE: first give each plain mesh under root whose geometry another
 * plain mesh there also draws a copy of its own (MeshInstance3D primitives share one unit shape per tree, so tiles,
 * cards and tokens built from PLANE or BOX primitives draw one geometry: without this, faces of two copies of one
 * geometry are left as they are, since moving the geometry moves both). The copies cost memory and the meshes no
 * longer draw as one instanced batch. */
int cw_coplanar_separate_ex(t3_object *root, float eps, int *left, unsigned flags);
/* the same for a model's own parts, in its own space (geometries drawn together at one transform: a model's surface,
 * trim and glow), before it is placed anywhere; every mesh drawing these geometries takes the change */
int cw_coplanar_separate_model(t3_geometry *const *geos, int n, float eps);
/* the debug command and fields (call in init) */
void cw_coplanar_init(void);
/* where the switches (COPLANAR_CONTROL, COPLANAR_SEPARATE, COPLANAR_SWEEP, ...) are read: the environment by default; a
 * game whose checks run as a cart passes its own run options (a run-option table set over the debug channel). Call it
 * before cw_coplanar_init. */
void cw_coplanar_set_options(const char *(*get)(const char *));
/* COPLANAR_SWEEP as the options hold it now (cw_coplanar_init reads it once; a game that sets it later over its debug channel
 * calls this): the sweep's frames count from this call */
void cw_coplanar_sweep_arm(void);
/* how near two faces' planes must be to count as one plane (m; 1 mm by default): what the game's depth buffer cannot
 * tell apart at the distances it is seen from. A game whose camera resolves far finer (a table a metre from the eye)
 * sets less, so faces it lays a fraction of a millimetre apart on purpose are not counted. */
void cw_coplanar_set_plane_tol(float m);
/* a camera that sees far with a near plane close in (a skate park's town out to the horizon): the depth buffer tells two
 * depths d metres away apart only about d^2 / (near * 2^24) apart, so faces within four such steps of one plane count
 * (seen from within reach metres of center), and cw_coplanar_separate lays them at least twice that apart */
void cw_coplanar_set_view(float near, t3_vec3 center, float reach);
/* the vertices moved so far (what level.coplanar_moved shows; cw_coplanar_separate adds to it) */
int cw_coplanar_moved_total(void);
/* add to that count (a game that lays faces apart itself) */
void cw_coplanar_add_moved(int n);
/* how long the last cw_coplanar_separate took (ms, the search after it for *left not counted) */
double cw_coplanar_last_ms(void);

#ifdef __cplusplus
}
#endif
#endif

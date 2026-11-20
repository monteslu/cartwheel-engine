/* Static batches (cartwheel/static_batch.h): what src/scene/render3d.c calls. */
#ifndef CW_STATIC_BATCH_INTERNAL_H
#define CW_STATIC_BATCH_INTERNAL_H

#include "cartwheel/scene.h"
#include "three.h"

typedef struct cw__sbatch_rec cw__sbatch_rec;
/* Each frame for a MeshInstance3D with batch = true: the meshes under root (the node's drawn object) draw through
 * the tree's batches; *slot is the node's record (made, updated, or rebuilt when its meshes changed). */
void cw__sbatch_sync(cw_tree *t, cw__sbatch_rec **slot, t3_object *root, bool cast, bool receive);
/* The node draws on its own again (batch turned off, the node freed). */
void cw__sbatch_remove(cw__sbatch_rec **slot);

#endif

/* Shadow caster culling for one view's render (src/render/shadow_cull.c). */
#ifndef CW_SHADOW_CULL_H
#define CW_SHADOW_CULL_H

#include "three.h"

void cw__shadow_cull_begin(t3_scene *scene, const t3_camera *cam); /* small casters stop casting */
void cw__shadow_cull_end(void);                                     /* they cast again */

#endif

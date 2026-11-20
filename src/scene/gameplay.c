/* The gameplay node types (cartwheel/gameplay.h): one registration call for the scene module. */
#include "cartwheel/gameplay.h"

void cw__register_timer_type(void);
void cw__register_path_types(void); /* path_nodes.c: Path2D/3D, PathFollow2D/3D (cartwheel/curve.h) */
void cw__register_notifier_types(void);  /* notifier.c: VisibleOnScreenNotifier2D/3D */
void cw__register_spring_arm_type(void); /* spring_arm.c: SpringArm3D */
void cw__register_gameplay_types(void); /* called by cw_scene_register_builtin_types (nodes.c) */

void cw__register_gameplay_types(void) {
  cw__register_timer_type();
  cw__register_path_types();
  cw__register_notifier_types();
  cw__register_spring_arm_type();
}

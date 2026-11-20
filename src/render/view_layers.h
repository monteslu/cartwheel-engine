/* One-pass split screen (cw_pipeline_set_views_array): geometry drawn for one view only (a terrain's patches, its
 * scatter, each selected and posed for that view's camera) sits on that view's layer, the top four of three.c's 32,
 * and each view's camera sees its own (and none of the other views') in that run. Outside such a run the geometry is
 * on layer 0 as before. */
#ifndef CW_VIEW_LAYERS_H
#define CW_VIEW_LAYERS_H
#include <stdint.h>

#define CW__VIEW_LAYERS 0xF0000000u
static inline uint32_t cw__view_layer(int view) { return 1u << (28 + view); }

#endif

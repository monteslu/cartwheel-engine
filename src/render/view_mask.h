/* Per-view visibility for split screen (view_mask.c; cw_object_set_views, cartwheel/pipeline.h). Internal. */
#ifndef CW_VIEW_MASK_H
#define CW_VIEW_MASK_H
#include <stdbool.h>
#include <stdint.h>

#include "three.h"

int cw__view_masks_count(void);
/* once a frame, before it draws: masked objects gone from every scene are let go */
void cw__view_masks_frame(void);
/* a frame drawn view by view: view k's masks as visibility (before its run), the game's back after the views */
void cw__view_masks_apply_visible(int view);
void cw__view_masks_restore_visible(void);
/* a frame drawn in one pass over n views: the masks as view layers; cam_layers[k] is view k's camera's layers without
 * the view layers, cam_view_layer[k] whether that camera sees its view layer. Returns why the frame cannot (nothing
 * changed then), NULL when applied. */
const char *cw__view_masks_apply_layers(int n, const uint32_t *cam_layers, const bool *cam_view_layer);
void cw__view_masks_restore_layers(void);
/* inside a one-pass run (after cw__view_masks_apply_layers): o and its subtree drawn only in the views of views as well,
 * until the run ends (the VisibilityManager's per-view culling). False when it cannot be (the frame must not stay in
 * one pass) */
bool cw__view_masks_draw_in(t3_object *o, uint32_t views);

#endif

/* render2d's video-frame converter (yuv.c), for src/video: YUV 4:2:0
 * planes into an RGB render target on the GPU. Not public. */
#ifndef CW_R2D_YUV_H
#define CW_R2D_YUV_H

#include "cartwheel/render2d.h"

typedef struct cw__yuv cw__yuv;
/* A w x h target and three planes: luma yw x yh, chroma half that (sizes as
 * the decoder stores them, padded to whole macroblocks). NULL (logged) when
 * the GPU objects could not be made. */
cw__yuv *cw__yuv_new(int w, int h, int yw, int yh);
cw_texture cw__yuv_target(const cw__yuv *y);
/* Uploads the planes (tightly packed, the sizes above) and converts them
 * into the target. Main thread, outside a render2d pass. */
void cw__yuv_convert(cw__yuv *y, const uint8_t *const planes[3]);
void cw__yuv_free(cw__yuv *y);

#endif

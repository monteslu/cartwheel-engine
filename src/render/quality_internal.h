/* What the renderer asks of the quality preset (cartwheel/quality.h). */
#ifndef CW_QUALITY_INTERNAL_H
#define CW_QUALITY_INTERNAL_H

#include "cartwheel/pipeline.h"

void cw__quality_init(void);                  /* the platform, before the app's init */
void cw__quality_post(cw_post_settings *s);   /* the frame's post settings, capped / raised */
int cw__quality_shadow_size(int authored);    /* a light's shadow map size */
bool cw__quality_soft_shadows(void);          /* soft PCF (else plain PCF) */
int cw__quality_shadow_level(void);           /* the Shadows setting: 0 Low .. 3 Ultra */
float cw__quality_lod_bias(void);             /* the LOD error budget's multiplier */

#endif

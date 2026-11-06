/* What the renderer asks of the quality preset (cartwheel/quality.h). */
#ifndef CW_QUALITY_INTERNAL_H
#define CW_QUALITY_INTERNAL_H

#include "cartwheel/pipeline.h"

void cw__quality_init(void);                  /* the platform, before the app's init */
void cw__quality_post(cw_post_settings *s);   /* the frame's post settings, capped / raised */
int cw__quality_shadow_size(int authored);    /* a light's shadow map size */
bool cw__quality_soft_shadows(void);          /* soft PCF (else plain PCF) */
int cw__quality_shadow_level(void);           /* the Shadows setting: CW_GFX_SHADOWS_MINIMAL .. ULTRA */
int cw__quality_cascades(int authored);       /* the sun's cascades: at most 1, 2, 3 on Minimal, Low, Medium */
float cw__quality_shadow_distance(void);      /* the cascades' distance scale: x0.5 Minimal, x1.5 Ultra */
bool cw__quality_local_shadows(void);         /* omni and spot lights cast shadows (not on Minimal) */
float cw__quality_caster_min(void);           /* a caster's least on-screen size (radius / distance; 0 = every caster) */
float cw__quality_lod_bias(void);             /* the LOD error budget's multiplier */
int cw__quality_effects(void);                /* effects detail as drawn (a cw_quality) */
bool cw__quality_small_hdr(void);             /* effects detail Low and Very Low: the HDR scene in R11G11B10 float */
bool cw__quality_light_bloom(void);           /* effects detail Low: bloom's first level from four taps, not thirteen */
bool cw__quality_fused_post(void);            /* effects detail Low and Very Low: the height fog and FXAA fused into the tonemap pass */

#endif

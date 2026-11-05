// three.c generator extension "waterlow" (state standard+xwaterlow and its light variants): Water3D's surface on
// effects detail Very Low (cartwheel/quality.h): the waves, one normal map octave, the floor's depth colour and the
// shore, crest and wake foam, without the refraction, the ripples' slope, caustics or the foam's lace. The TSL lives in
// water_shading.mjs.
import { applyWater } from './water_shading.mjs';
export const name = 'waterlow';
export const apply = (ctx) => applyWater(ctx, true);

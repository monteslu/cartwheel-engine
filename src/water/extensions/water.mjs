// three.c generator extension "water" (state standard+xwater and its light variants, depth+xwater for shadows):
// Water3D's surface (cartwheel/water.h). The TSL lives in water_shading.mjs.
import { applyWater } from './water_shading.mjs';
export const name = 'water';
export const apply = (ctx) => applyWater(ctx, false);

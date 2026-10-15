// three.c generator extension "terrainfar" (state standard+inst+xterrainfar and its
// variants): cartwheel's terrain shading for Patches wholly past the far distance: the baked macro colour and the normal map only (two fetches).
// The TSL lives in terrain_shading.mjs.
import { applyTerrain } from './terrain_shading.mjs';
export const name = 'terrainfar';
export const apply = (ctx) => applyTerrain(ctx, 'far');

// three.c generator extension "terrain" (state standard+inst+xterrain and its
// variants): cartwheel's terrain shading for High and Ultra: four control texels blended bilinearly, height blending, triplanar on steep slopes.
// The TSL lives in terrain_shading.mjs.
import { applyTerrain } from './terrain_shading.mjs';
export const name = 'terrain';
export const apply = (ctx) => applyTerrain(ctx, 'high');

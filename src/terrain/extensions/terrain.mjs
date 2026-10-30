// three.c generator extension "terrain" (state standard+inst+xterrain and its
// variants): cartwheel's terrain shading for High and Ultra: the same two layers weighted from four control texels as
// terrainlow, height blending, triplanar on steep slopes (the quality uniform switches it on).
// The TSL lives in terrain_shading.mjs.
import { applyTerrain } from './terrain_shading.mjs';
export const name = 'terrain';
export const apply = (ctx) => applyTerrain(ctx, 'high');

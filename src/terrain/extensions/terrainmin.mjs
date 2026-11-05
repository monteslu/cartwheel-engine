// three.c generator extension "terrainmin" (state standard+inst+xterrainmin and its
// variants): cartwheel's terrain shading for Very Low: as terrainlow, with each layer read once (no
// stochastic tiling: a third of the reads). The TSL lives in terrain_shading.mjs.
import { applyTerrain } from './terrain_shading.mjs';
export const name = 'terrainmin';
export const apply = (ctx) => applyTerrain(ctx, 'min');

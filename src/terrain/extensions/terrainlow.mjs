// three.c generator extension "terrainlow" (state standard+inst+xterrainlow and its
// variants): cartwheel's terrain shading for Low and Medium: the nearest control texel; triplanar on steep slopes at Medium.
// The TSL lives in terrain_shading.mjs.
import { applyTerrain } from './terrain_shading.mjs';
export const name = 'terrainlow';
export const apply = (ctx) => applyTerrain(ctx, 'low');

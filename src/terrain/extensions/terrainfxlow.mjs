// The near terrain with its near-ground options (terrain_shading.mjs, fx): what terrain.mjs is, plus uv rotation
// and scale read and blended across control texels, macro variation and parallax occlusion. Drawn instead of it
// only while a terrain uses one of them (terrain_render.c, near_ext), so a terrain that uses none pays nothing.
import { applyTerrain } from './terrain_shading.mjs';
export const name = 'terrainfxlow';
export const apply = (ctx) => applyTerrain(ctx, 'low', true);

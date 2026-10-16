// the "csm" shading part (cartwheel/shading.h): the sun's cascaded shadows, alone as xcsm,
// composed as xcluster_csm, xcsm_gi, xcluster_csm_gi
import { apply as applyPart } from './csm_part.mjs';
export const name = 'csm';
export const apply = (ctx) => applyPart(ctx);

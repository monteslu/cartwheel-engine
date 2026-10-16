// the "cluster" shading part (cartwheel/shading.h): alone as xcluster, composed as xcluster_gi
import { apply as applyPart } from './cluster_part.mjs';
export const name = 'cluster';
export const apply = (ctx) => applyPart(ctx);

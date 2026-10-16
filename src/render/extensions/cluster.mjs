// TEMPORARY (until the shading-part composer lands): the "cluster" part alone as extension xcluster
import { apply as applyPart } from './cluster_part.mjs';
export const name = 'cluster';
export const apply = (ctx) => applyPart(ctx);

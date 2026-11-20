// the "clusteries" shading part (cartwheel/shading.h, cartwheel/lighting.h): the clustered lights with IES
// profiles. The same loop as "cluster" (cluster_part.mjs); a light whose row says so takes its shape from the
// profile atlas instead of a cone. Materials carry it in place of "cluster" while a clustered light of the
// scene has a profile.
import { apply as applyPart } from './cluster_part.mjs';
export const name = 'clusteries';
export const apply = (ctx) => applyPart(ctx, undefined, { ies: true });

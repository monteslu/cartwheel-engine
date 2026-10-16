// three.c generator extension "gi" (states like standard+senv+xgi, and
// composed after other parts: xterrain_gi, xcluster_gi; cartwheel/shading.h):
// reflection probes and the light probe grid for every lit surface. The TSL
// lives in gi_shading.mjs. A shadow caster (depth+xgi) draws as plain depth:
// nothing here changes a surface's shape.
import { giShading } from './gi_shading.mjs';
export const name = 'gi';
export const apply = (ctx) => (ctx.kind === 'depth' ? undefined : giShading(ctx));

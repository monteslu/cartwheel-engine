// water-surface.mjs on the VIEW grid with a wave mask (samples/water_open_sea).
process.argv.push('--app', 'water_open_sea');
await import('./water-surface.mjs');

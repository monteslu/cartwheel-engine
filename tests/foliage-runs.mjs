// Helper for tests/foliage.mjs and by hand: run samples/foliage_field once with a set of switches and print its
// fol.* debug fields as JSON.
//   node tests/foliage-runs.mjs <host> <gpu> <frames> '<env json>' [shot.png]
import { spawnSync } from 'node:child_process';
import { readFileSync, mkdirSync } from 'node:fs';
import { join, resolve, dirname } from 'node:path';
import { fileURLToPath } from 'node:url';

const ROOT = resolve(dirname(fileURLToPath(import.meta.url)), '..');
const OUT = join(ROOT, 'out', 'foliage');
mkdirSync(OUT, { recursive: true });

export function runField(host, gpu, frames, env, name, { shot = true, app = 'foliage_field', cmds } = {}) {
  const png = join(OUT, `${name}.${host}.${gpu}.png`), st = join(OUT, `${name}.${host}.${gpu}.json`);
  const a = [join(ROOT, 'tools', 'run-cart.mjs'), '--host', host, '--app', app, '--frames', String(frames), '--gpu', gpu, '--state', st];
  if (shot) a.push('--shot', png);
  if (env && Object.keys(env).length) a.push('--env', JSON.stringify(env));
  if (cmds) a.push('--start-cmds', cmds.join(';'));
  const r = spawnSync('node', a, { encoding: 'utf8', env: process.env, maxBuffer: 64 << 20 });
  if (r.status !== 0) throw new Error(`${host} ${gpu} ${name}: ${((r.stderr || '') + (r.stdout || '')).trim().split('\n').slice(-3).join(' | ')}`);
  const j = JSON.parse(readFileSync(st, 'utf8'));
  const f = j.fields || j.state || j;
  return { fields: f, png };
}

if (import.meta.url === `file://${process.argv[1]}`) {
  const [host, gpu, frames, envj, name] = process.argv.slice(2);
  const { fields, png } = runField(host, gpu, +frames, JSON.parse(envj || '{}'), name || 'manual');
  const pick = {};
  for (const [k, v] of Object.entries(fields)) if (k.startsWith('fol.') || k === 'render.skipped' || k === 'cw.three.missing') pick[k] = v;
  console.log(JSON.stringify(pick), png);
}

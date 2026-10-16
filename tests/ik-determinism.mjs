// IK determinism across hosts AND job-pool sizes: the unit-ik and anim-ik
// gates (tests/gates.json) on native and Chromium at CW_WORKERS=0, 3 and 7.
// Each gate holds Chromium to the native run; this script then holds every
// pool size to the first one: unit-ik's checksum and anim-ik's pose hashes
// (the front robot and the four-robot crowd, whose solves run one model per
// job) must be the same bits at every pool size, and must differ from
// anim-ik's control run (foot placement off), or the hashes see nothing.
//
// CW_WORKERS sizes the pool natively; Chromium's carts get it as
// navigator.hardwareConcurrency (tools/lib/workers.mjs).
//
//   node tests/ik-determinism.mjs [extra gate.mjs args]
import { spawn } from 'node:child_process';
import { readFileSync } from 'node:fs';
import { join, resolve, dirname } from 'node:path';
import { fileURLToPath } from 'node:url';
import { enterGpuSandbox } from '../tools/lib/gpu-sandbox.mjs';
import { takeSuiteLock } from '../tools/lib/suite-lock.mjs';
await enterGpuSandbox(); // only the test GPU is visible, no desktop (tools/lib/gpu-sandbox.mjs)
await takeSuiteLock('ik-determinism'); // the gate runs below inherit it

const ROOT = resolve(dirname(fileURLToPath(import.meta.url)), '..');
const extra = process.argv.slice(2);
const HOSTS = ['native', 'chromium'];
const PICK = { 'unit-ik': ['unit.checksum', 'unit.passed'], 'anim-ik': ['hash_win', 'crowd_win', 'foot_err'] };
const state = (gate, host) => {
  const j = JSON.parse(readFileSync(join(ROOT, 'out', 'gate', `${gate}.${host}.json`), 'utf8'));
  return j.state || j;
};
let fail = 0, ref = null;
for (const w of ['0', '3', '7']) {
  const env = { ...process.env, CW_WORKERS: w };
  console.log(`── CW_WORKERS=${w}`);
  const code = await new Promise((res) => spawn('node', [join(ROOT, 'tests', 'gate.mjs'), 'unit-ik', 'anim-ik',
    '--hosts', HOSTS.join(','), ...extra], { stdio: 'inherit', env }).on('close', (c) => res(c ?? 1)));
  if (code !== 0) { fail++; continue; }
  const got = {};
  for (const [gate, fields] of Object.entries(PICK))
    for (const host of HOSTS) {
      const s = state(gate, host);
      for (const f of fields) got[`${gate}.${host}.${f}`] = JSON.stringify(s[f]);
    }
  // the control: the hashes must see a change (anim-ik's control run turns foot placement off)
  const ctl = state('anim-ik', 'control');
  if (JSON.stringify(ctl.crowd_win) === got['anim-ik.native.crowd_win']) { console.log('  CONTROL PASSED: the crowd hashes did not change with foot placement off'); fail++; }
  if (!ref) { ref = got; continue; }
  for (const k of Object.keys(ref))
    if (ref[k] !== got[k]) { console.log(`  ${k}: ${got[k]} at CW_WORKERS=${w} vs ${ref[k]} at 0`); fail++; }
}
console.log(fail ? `ik-determinism: ${fail} difference(s)` : 'ok   ik-determinism (native, chromium at CW_WORKERS 0, 3 and 7)');
process.exit(fail ? 1 : 0);

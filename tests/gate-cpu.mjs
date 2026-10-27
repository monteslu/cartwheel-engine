// `tests/gate.mjs --cpu`: the CPU-only gate mode never takes the GPU suite lock and never enters the GPU sandbox.
//   run:      with the lock held by another process, `gate.mjs --cpu <gate>` finishes and prints "ok" (it did not queue)
//   control:  with the same lock held, the plain `gate.mjs <gate>` is still waiting after the same time
//             ("waiting for the GPU suite lock"): so the first result is the mode's doing, not an idle lock
// The lock lives in a throw-away XDG_RUNTIME_DIR, so a real suite is never held up. No GPU is used by this test
// (the --cpu run is on Mesa's software renderer; the control run never gets past the lock).
import { spawn, spawnSync } from 'node:child_process';
import { mkdtempSync, rmSync, existsSync } from 'node:fs';
import { join, resolve, dirname } from 'node:path';
import { fileURLToPath } from 'node:url';
import { tmpdir } from 'node:os';

const ROOT = resolve(dirname(fileURLToPath(import.meta.url)), '..');
const GATE = 'platformer2d';
if (!existsSync(join(ROOT, 'build', 'native', 'samples', GATE))) { console.error(`FAIL build ${GATE} first`); process.exit(1); }
const runtime = mkdtempSync(join(process.env.CW_SCRATCH || tmpdir(), 'gate-cpu-'));
const env = { ...process.env, XDG_RUNTIME_DIR: runtime };
delete env.CW_SUITE_LOCK_HELD; delete env.CW_TEST_GPU;
let bad = 0;
const check = (name, ok, extra = '') => { console.log(`${ok ? 'ok  ' : 'FAIL'} ${name}${ok ? '' : ': ' + extra}`); if (!ok) bad++; };

// a process that holds the suite lock until killed
const holder = spawn('node', ['--input-type=module', '-e',
  `import { takeSuiteLock } from ${JSON.stringify(join(ROOT, 'tools/lib/suite-lock.mjs'))}; await takeSuiteLock('holder'); console.log('held'); setInterval(() => {}, 1000);`],
  { env, stdio: ['ignore', 'pipe', 'inherit'] });
await new Promise((res) => holder.stdout.once('data', res));
try {
  const r = spawnSync('node', [join(ROOT, 'tests', 'gate.mjs'), '--cpu', GATE], { env, encoding: 'utf8', timeout: 120000 });
  check('--cpu runs while another suite holds the lock', r.status === 0 && new RegExp(`ok\\s+${GATE} \\(native\\)`).test(r.stdout) && !/waiting for the GPU suite lock/.test(r.stderr),
    `status ${r.status} ${r.stdout}${r.stderr}`);
  // CONTROL: the plain gate queues behind the held lock. CW_TEST_GPU=desktop skips the GPU sandbox, which needs a
  // pinned test GPU (CI has none) and comes before the lock; the gate it is given does not exist, so even a run
  // that got past the lock would draw nothing on any GPU.
  const waiting = await new Promise((res) => {
    const p = spawn('node', [join(ROOT, 'tests', 'gate.mjs'), 'gate-cpu-no-such-gate'], { env: { ...env, CW_TEST_GPU: 'desktop' }, stdio: ['ignore', 'ignore', 'pipe'] });
    let err = '';
    p.stderr.on('data', (d) => { err += d; if (/waiting for the GPU suite lock/.test(err)) { p.kill('SIGKILL'); res(true); } });
    p.on('close', () => res(/waiting for the GPU suite lock/.test(err)));
    setTimeout(() => { p.kill('SIGKILL'); res(false); }, 20000);
  });
  check('CONTROL: the plain gate waits on the same lock', waiting);
} finally {
  holder.kill('SIGKILL');
  rmSync(runtime, { recursive: true, force: true });
}
process.exit(bad ? 1 : 0);

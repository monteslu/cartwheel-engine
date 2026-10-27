// The GPU suite lock (tools/lib/suite-lock.mjs): suites are served in arrival order, and no suite holds the lock
// past CW_SUITE_MAX_MINUTES. Runs against a private lock directory (XDG_RUNTIME_DIR in a temp dir), never the real lock,
// and needs no GPU.
//   hold limit:  a holder with a child that sleeps past the limit is stopped (exit 124, the message), the child dies and the
//                lock directory is gone
//   control:     the same holder inside the limit finishes by itself (exit 0), its child is not killed, so the limit check is
//                the limit and not a coincidence
//   fairness:    with the lock held, A then B queue; A runs before B (and a control with the queue order swapped runs B first)
import { spawn, spawnSync } from 'node:child_process';
import { mkdtempSync, existsSync, rmSync } from 'node:fs';
import { join, resolve, dirname } from 'node:path';
import { tmpdir } from 'node:os';
import { fileURLToPath } from 'node:url';

const ROOT = resolve(dirname(fileURLToPath(import.meta.url)), '..');
const LOCK = join(ROOT, 'tools', 'lib', 'suite-lock.mjs');
let bad = 0;
const check = (name, ok, extra = '') => { console.log(`${ok ? 'ok  ' : 'FAIL'} ${name}${ok ? '' : ': ' + extra}`); if (!ok) bad++; };
const env = (rt, extra = {}) => { const e = { ...process.env, XDG_RUNTIME_DIR: rt, ...extra }; delete e.CW_SUITE_LOCK_HELD; return e; };
const holderSrc = (name, work) => `import(${JSON.stringify(LOCK)}).then(async (m) => { await m.takeSuiteLock(${JSON.stringify(name)}); ${work} })`;
// holds the lock, starts a child that sleeps `sleepS` seconds, waits for it
const holder = (name, sleepS) => holderSrc(name, `const { spawn } = await import('node:child_process'); await new Promise((r) => spawn('sleep', ['${sleepS}']).on('close', r)); console.log('done ${name}', Date.now());`);

{ // hold limit
  const rt = mkdtempSync(join(tmpdir(), 'suite-lock-'));
  const t0 = Date.now();
  const r = spawnSync(process.execPath, ['--input-type=module', '-e', holder('hog', 30)], { encoding: 'utf8', timeout: 60000, env: env(rt, { CW_SUITE_MAX_MINUTES: '0.04' }) });
  check('a holder past the limit is stopped with exit 124', r.status === 124, `status ${r.status} ${r.stderr}`);
  check('and the message says why and what to do', /CW_SUITE_MAX_MINUTES/.test(r.stderr) && /Split the work/.test(r.stderr), r.stderr);
  check('it stopped within a few seconds of the limit, not at the child\'s end', Date.now() - t0 < 15000, `${Date.now() - t0} ms`);
  check('the lock directory was released', !existsSync(join(rt, 'cartwheel-gpu-suite.lock')), 'still there');
  const c = spawnSync(process.execPath, ['--input-type=module', '-e', holder('quick', 1)], { encoding: 'utf8', timeout: 60000, env: env(rt, { CW_SUITE_MAX_MINUTES: '0.5' }) });
  check('CONTROL: a holder inside the limit finishes by itself (exit 0)', c.status === 0 && /done quick/.test(c.stdout), `status ${c.status} ${c.stderr}`);
  rmSync(rt, { recursive: true, force: true });
}

{ // arrival order
  const order = async (first, second) => {
    const rt = mkdtempSync(join(tmpdir(), 'suite-lock-'));
    const run = (name, sleepS) => new Promise((res) => {
      let out = '';
      const p = spawn(process.execPath, ['--input-type=module', '-e', holder(name, sleepS)], { env: env(rt, { CW_SUITE_MAX_MINUTES: '5' }) });
      p.stdout.on('data', (d) => { out += d; });
      p.on('close', () => res(out));
    });
    const wait = (ms) => new Promise((r) => setTimeout(r, ms));
    const h = run('holder', 3);
    await wait(800);
    const a = run(first, 1);
    await wait(800);
    const b = run(second, 1);
    const [, ao, bo] = await Promise.all([h, a, b]);
    rmSync(rt, { recursive: true, force: true });
    const t = (o, n) => Number((new RegExp(`done ${n} (\\d+)`).exec(o) || [])[1]);
    return [t(ao, first), t(bo, second)];
  };
  const [ta, tb] = await order('A', 'B');
  check('waiting suites run in arrival order (A queued first runs first)', ta > 0 && tb > 0 && ta < tb, `${ta} ${tb}`);
  const [tb2, ta2] = await order('B', 'A');
  check('CONTROL: queue them the other way round and the order follows the arrival, not the name', tb2 > 0 && ta2 > 0 && tb2 < ta2, `${tb2} ${ta2}`);
}
process.exit(bad ? 1 : 0);

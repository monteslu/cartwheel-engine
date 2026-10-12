// The GPU guards (tools/lib/gpu-guard.mjs), each with a must-fail control:
//   kernel   an injected amdgpu error line (CW_GPU_GUARD_FAKE_KERNEL_FILE) kills the
//            children, frees the suite lock, prints the line and exits 3;
//            control: unrelated lines and another card's error do NOT fire it
//   start    VRAM above the start budget (a fake reading) makes a gate wait,
//            then exit 3; control: a low reading starts the gate
//   during   VRAM rising above the abort budget during a gate exits 3;
//            control: a steady low reading lets the gate finish
//   account  a real native GL run (2 s of it) is seen in the per-process fdinfo accounting
//            (gpu-mem.json peak > 0); control: a gate that runs a process with no
//            GPU records none
//   leak     the growth summary flags a monotonic climb across gates;
//            control: a flat series is not flagged
import { spawn } from 'node:child_process';
import { appendFileSync, existsSync, mkdirSync, mkdtempSync, readFileSync, writeFileSync, rmSync } from 'node:fs';
import { join, resolve, dirname } from 'node:path';
import { fileURLToPath } from 'node:url';
import { tmpdir } from 'node:os';

const ROOT = resolve(dirname(fileURLToPath(import.meta.url)), '..');
const HERE = fileURLToPath(import.meta.url);

// ── child mode: arm the guard and do something ───────────────────────────
if (process.argv[2] === '--child') {
  const { armGuard } = await import('../tools/lib/gpu-guard.mjs');
  const { takeSuiteLock } = await import('../tools/lib/suite-lock.mjs');
  const mode = process.argv[3];
  const sleep = (ms) => new Promise((r) => setTimeout(r, ms));
  await takeSuiteLock('gpu-guard-child');
  const guard = armGuard('gpu-guard-child', { outFile: process.env.GUARD_OUT });
  console.log('ARMED');
  if (mode === 'kernel') {
    const s = spawn('sleep', ['120'], { stdio: 'ignore' });
    console.log('SLEEPER ' + s.pid);
    await sleep(+process.env.GUARD_HOLD_MS || 20000);
    console.log('COMPLETED');
  } else if (mode === 'gate') {
    await guard.gate('probe');
    console.log('GATE STARTED');
    await sleep(+process.env.GUARD_HOLD_MS || 2500);
    console.log('COMPLETED');
  } else if (mode === 'account') {
    await guard.gate('account');
    await new Promise((res) => spawn(process.env.GUARD_CMD, JSON.parse(process.env.GUARD_ARGS || '[]'), { stdio: 'ignore' }).on('close', res));
    guard.finish();
    console.log('COMPLETED');
  } else if (mode === 'leak') {
    for (const v of JSON.parse(process.env.GUARD_SERIES)) {
      writeFileSync(process.env.CW_GPU_GUARD_FAKE_VRAM_FILE, String(v));
      await guard.gate('g' + v);
      guard.end();
    }
    const gr = guard.finish();
    console.log('LEAK ' + (gr ? gr.leak : 'none'));
  }
  guard.finish?.();
  process.exit(0);
}

// ── the test ─────────────────────────────────────────────────────────────
const { testGpu } = await import('../tools/lib/wgpu-pin.mjs');
const gpu = testGpu();
const W = mkdtempSync(join(ROOT, 'out', 'gpu-guard-'));
mkdirSync(W, { recursive: true });
let fail = 0;
const check = (name, ok, why = '') => { console.log(`${ok ? 'ok  ' : 'FAIL'} ${name}${ok ? '' : ': ' + why}`); if (!ok) fail++; };

function child(mode, env = {}, during) {
  const xdg = mkdtempSync(join(tmpdir(), 'cw-guard-xdg-'));
  const e = { ...process.env, XDG_RUNTIME_DIR: xdg, ...env };
  delete e.CW_SUITE_LOCK_HELD;
  const p = spawn('node', [HERE, '--child', mode], { env: e, stdio: ['ignore', 'pipe', 'pipe'] });
  let out = '';
  p.stdout.on('data', (d) => (out += d));
  p.stderr.on('data', (d) => (out += d));
  const t0 = Date.now();
  return new Promise((res) => {
    const poll = setInterval(() => { if (during && out.includes('ARMED') && !during.done) { during.done = true; during.fn(out); } }, 100);
    const kill = setTimeout(() => p.kill('SIGKILL'), 60000);
    p.on('close', (code) => { clearInterval(poll); clearTimeout(kill); res({ code, out, ms: Date.now() - t0, lock: existsSync(join(xdg, 'cartwheel-gpu-suite.lock')) }); });
  });
}
const alive = (pid) => { try { process.kill(pid, 0); return true; } catch { return false; } };
const kfile = join(W, 'kernel.log'), vfile = join(W, 'vram');
const total = gpu.vramTotal || 8 * 2 ** 30;

// kernel: an injected page fault on the pinned card
{
  writeFileSync(kfile, '');
  const bad = `[ 1234.5] amdgpu ${gpu.pci}: [gfxhub] page fault (src_id:0 ring:24 vmid:8 pasid:32777)\n`;
  const r = await child('kernel', { CW_GPU_GUARD_FAKE_KERNEL_FILE: kfile }, { fn: () => appendFileSync(kfile, bad) });
  const pid = +(/SLEEPER (\d+)/.exec(r.out) || [])[1];
  check('kernel: an injected page fault exits 3', r.code === 3, `exit ${r.code}: ${r.out.slice(-300)}`);
  check('kernel: the line is printed', r.out.includes('page fault'), r.out.slice(-300));
  check('kernel: no further work ran', !r.out.includes('COMPLETED'), r.out.slice(-300));
  check('kernel: the child process was killed', pid > 0 && !alive(pid), 'sleeper still alive');
  if (pid && alive(pid)) process.kill(pid, 'SIGKILL');
  check('kernel: the suite lock was released', !r.lock, 'lock directory remained');
  for (const [name, line] of [
    ['ring timeout', `amdgpu ${gpu.pci}: ring gfx_0.0.0 timeout, signaled seq=123, emitted seq=125`],
    ['command submission', '[drm] *ERROR* Not enough memory for command submission!'],
    ['GPU reset', `amdgpu ${gpu.pci}: amdgpu: GPU reset begin!`],
  ]) {
    writeFileSync(kfile, '');
    const q = await child('kernel', { CW_GPU_GUARD_FAKE_KERNEL_FILE: kfile, GUARD_HOLD_MS: '15000' }, { fn: () => appendFileSync(kfile, `[ 9.9] ${line}\n`) });
    check(`kernel: ${name} exits 3`, q.code === 3, `exit ${q.code}: ${q.out.slice(-200)}`);
  }
  // control: nothing that is a GPU error on the pinned card
  writeFileSync(kfile, '');
  const other = gpu.pci === '0000:fe:00.0' ? '0000:fd:00.0' : '0000:fe:00.0';
  const lines = `[ 1.0] usb 3-1.4: 1:2: usb_set_interface failed (-110)\n[ 2.0] amdgpu ${gpu.pci}: amdgpu: SMU is initialized successfully!\n[ 3.0] amdgpu ${other}: [gfxhub] page fault (src_id:0)\n`;
  const c = await child('kernel', { CW_GPU_GUARD_FAKE_KERNEL_FILE: kfile, GUARD_HOLD_MS: '1500' }, { fn: () => appendFileSync(kfile, lines) });
  check('kernel control: unrelated lines and another card\'s fault do not fire', c.code === 0 && c.out.includes('COMPLETED'), `exit ${c.code}: ${c.out.slice(-300)}`);
}

// start budget
{
  // the budget is free VRAM: 512 MiB reserve + 1280 MiB need = 1792 MiB free to start
  const freeOf = (mib) => String(Math.floor(total - mib * 2 ** 20));
  writeFileSync(vfile, freeOf(1700));
  const r = await child('gate', { CW_GPU_GUARD_FAKE_VRAM_FILE: vfile, CW_GPU_GUARD_WAIT_MS: '800' });
  check('start budget: 1700 MiB free never starts the gate and exits 3', r.code === 3 && !r.out.includes('GATE STARTED'), `exit ${r.code}: ${r.out.slice(-300)}`);
  // a desktop at ~75% of an 8 GiB card still leaves room: the old 60% share budget never started here
  writeFileSync(vfile, freeOf(1900));
  const c = await child('gate', { CW_GPU_GUARD_FAKE_VRAM_FILE: vfile, CW_GPU_GUARD_WAIT_MS: '800', GUARD_HOLD_MS: '800' });
  check('start budget control: 1900 MiB free starts the gate', c.code === 0 && c.out.includes('GATE STARTED'), `exit ${c.code}: ${c.out.slice(-300)}`);
  // a card that frees up while the gate waits is picked up
  writeFileSync(vfile, freeOf(1000));
  const w = await child('gate', { CW_GPU_GUARD_FAKE_VRAM_FILE: vfile, CW_GPU_GUARD_WAIT_MS: '20000', GUARD_HOLD_MS: '500' },
    { fn: () => setTimeout(() => writeFileSync(vfile, freeOf(3000)), 700) });
  check('start budget: a gate waits for the card to free up and then runs', w.code === 0 && w.out.includes('GATE STARTED'), `exit ${w.code}: ${w.out.slice(-300)}`);
}

// abort budget during a gate
{
  writeFileSync(vfile, String(Math.floor(total * 0.1)));
  const r = await child('gate', { CW_GPU_GUARD_FAKE_VRAM_FILE: vfile, GUARD_HOLD_MS: '15000' },
    { fn: () => setTimeout(() => writeFileSync(vfile, String(Math.floor(total - 400 * 2 ** 20))), 1000) });
  check('abort budget: free VRAM under the 512 MiB reserve during a gate exits 3', r.code === 3 && !r.out.includes('COMPLETED') && r.ms < 14000, `exit ${r.code} after ${r.ms} ms: ${r.out.slice(-300)}`);
  check('abort budget: the suite lock was released', !r.lock, 'lock directory remained');
  // control: 600 MiB free is tight but above the reserve, and a gate that started there finishes
  writeFileSync(vfile, String(Math.floor(total * 0.1)));
  const c = await child('gate', { CW_GPU_GUARD_FAKE_VRAM_FILE: vfile, GUARD_HOLD_MS: '2000' },
    { fn: () => setTimeout(() => writeFileSync(vfile, String(Math.floor(total - 600 * 2 ** 20))), 500) });
  check('abort budget control: 600 MiB free (above the reserve) finishes the gate', c.code === 0 && c.out.includes('COMPLETED'), `exit ${c.code}: ${c.out.slice(-300)}`);
}

// accounting: a real native GL run shows up per process
{
  const out = join(W, 'mem-real.json');
  // a headless test run is uncapped (200k frames in under 3 s), so a frame count would end it between two
  // 250 ms samples: run an endless one for 2 s instead (timeout signals the whole process group)
  const real = await child('account', { GUARD_OUT: out, GUARD_CMD: 'timeout', GUARD_ARGS: JSON.stringify(['2', 'node', join(ROOT, 'tools/run-cart.mjs'), '--host', 'native', '--app', 'hello', '--frames', '1000000000']) });
  let j = null;
  try { j = JSON.parse(readFileSync(out, 'utf8')); } catch { /* reported below */ }
  const g = j && j.gates[0];
  const nat = g && g.hosts.native;
  check('accounting: the native run is attributed VRAM and GTT in fdinfo', real.code === 0 && nat && nat.vram > 0 && nat.gtt > 0, `exit ${real.code} ${JSON.stringify(g && g.hosts)} ${real.out.slice(-300)}`);
  check('accounting: the card\'s own reading is recorded', g && g.before.vram > 0 && g.peak.vram >= g.before.vram, JSON.stringify(g && g.peak));
  const out2 = join(W, 'mem-none.json');
  await child('account', { GUARD_OUT: out2, GUARD_CMD: 'sleep', GUARD_ARGS: '["1.5"]' });
  const j2 = JSON.parse(readFileSync(out2, 'utf8'));
  // the shared romdev server is always sampled (it hosts romdev runs), so it shows up whenever it is up
  const own2 = Object.keys(j2.gates[0].hosts).filter((h) => h !== 'romdev');
  check('accounting control: a process with no GPU is attributed nothing', own2.length === 0, JSON.stringify(j2.gates[0].hosts));
}

// leak signal across gates
{
  const mb = 2 ** 20;
  const climb = Array.from({ length: 12 }, (_, i) => Math.floor(total * 0.1) + i * 60 * mb);
  const flat = Array.from({ length: 12 }, () => Math.floor(total * 0.1));
  const r = await child('leak', { CW_GPU_GUARD_FAKE_VRAM_FILE: vfile, GUARD_OUT: join(W, 'leak.json'), GUARD_SERIES: JSON.stringify(climb) });
  check('leak: a monotonic climb across 12 gates is flagged', r.out.includes('LEAK true') && r.out.includes('LEAK SIGNAL'), r.out.slice(-300));
  const c = await child('leak', { CW_GPU_GUARD_FAKE_VRAM_FILE: vfile, GUARD_OUT: join(W, 'leak2.json'), GUARD_SERIES: JSON.stringify(flat) });
  check('leak control: a flat series is not flagged', c.out.includes('LEAK false') && !c.out.includes('LEAK SIGNAL'), c.out.slice(-300));
}

rmSync(W, { recursive: true, force: true });
process.exit(fail ? 1 : 0);

// No test run reaches the desktop: each host's run of r2d_sprites (native and
// wcnative, GL and WebGPU) is traced with strace -f, and no process of it may
// connect() to an X server (/tmp/.X11-unix/X<n>, path or abstract) or a
// Wayland compositor ($XDG_RUNTIME_DIR/wayland-<n>). The runs go through
// tools/run-cart.mjs as the suites do (the GPU sandbox, SDL offscreen, no
// display variables; tools/lib/gpu-sandbox.mjs, tools/lib/wgpu-pin.mjs).
// The trace must reach the player (its execve is in the log), or a clean
// result would only mean strace stopped at the sandbox (run alone, the bwrap
// exec is in it too; inside a suite the sandbox is already entered).
//   control  the detector sees a process that does connect to an X and a
//            Wayland socket path (no server needs to be listening)
import { spawnSync } from 'node:child_process';
import { existsSync, mkdtempSync, readFileSync, rmSync } from 'node:fs';
import { join, resolve, dirname } from 'node:path';
import { fileURLToPath } from 'node:url';

const ROOT = resolve(dirname(fileURLToPath(import.meta.url)), '..');
const strace = ['/usr/bin/strace', '/bin/strace'].find(existsSync);
if (!strace) { console.error('gpu-no-display needs strace'); process.exit(1); }
const W = mkdtempSync(join(ROOT, 'out', 'nodisplay-'));
let fail = 0;
const check = (name, ok, why = '') => { console.log(`${ok ? 'ok  ' : 'FAIL'} ${name}${ok ? '' : ': ' + why}`); if (!ok) fail++; };

const DISPLAY_SOCK = /connect\(\d+, \{sa_family=AF_UNIX, sun_path=@?"([^"]*(?:\.X11-unix\/X\d+|\/wayland-\d+))"/g;

/** run a command under strace -f; the display sockets it tried to connect to */
function traced(name, cmd, args, env = process.env) {
  const log = join(W, name + '.strace');
  const r = spawnSync(strace, ['-f', '-qq', '-e', 'trace=connect,execve', '-o', log, cmd, ...args], { env, encoding: 'utf8', timeout: 180000 });
  const t = existsSync(log) ? readFileSync(log, 'utf8') : '';
  return { status: r.status, err: (r.stderr || '').slice(-400), sockets: [...t.matchAll(DISPLAY_SOCK)].map((m) => m[1]), connects: (t.match(/connect\(/g) || []).length,
    execs: [...t.matchAll(/execve\("([^"]+)"/g)].map((m) => m[1].split('/').pop()) };
}

// control: the detector catches both kinds of desktop socket
{
  const run = process.env.XDG_RUNTIME_DIR || `/run/user/${process.getuid()}`;
  const js = `const net=require('net');for(const p of ['\\0/tmp/.X11-unix/X0','/tmp/.X11-unix/X0',${JSON.stringify(run + '/wayland-0')}]){const s=net.connect(p);s.on('error',()=>{});s.on('connect',()=>s.destroy())}setTimeout(()=>{},300)`;
  const c = traced('control', process.execPath, ['-e', js]);
  check('control: a connect to the X socket (abstract and path) and to Wayland is seen',
    c.sockets.some((s) => s.includes('.X11-unix')) && c.sockets.some((s) => s.includes('wayland-')), JSON.stringify(c));
}

// [leg, run-cart args, the player executable the trace must reach]
const legs = [
  ['native GL', ['--host', 'native'], 'r2d_sprites'],
  ['native WebGPU', ['--host', 'native', '--gpu', 'webgpu'], 'r2d_sprites'],
  ['wcnative GL', ['--host', 'wcnative'], 'wasmcart-run'],
  ['wcnative WebGPU', ['--host', 'wcnative', '--gpu', 'webgpu'], 'wasmcart-run'],
];
const env = { ...process.env };
delete env.CW_TEST_WINDOW;
delete env.CW_TEST_GPU;
for (const [name, a, player] of legs) {
  const r = traced(name.replace(/ /g, '-'), process.execPath, [join(ROOT, 'tools', 'run-cart.mjs'), ...a, '--app', 'r2d_sprites', '--frames', '30'], env);
  check(`${name}: the run passes`, r.status === 0, `exit ${r.status}: ${r.err}`);
  // inside a suite the sandbox is already entered (CW_GPU_SANDBOXED) and run-cart does not exec bwrap again
  const sandbox = process.env.CW_GPU_SANDBOXED ? true : r.execs.includes('bwrap');
  check(`${name}: the trace reaches the sandbox and ${player}`, sandbox && r.execs.includes(player), r.execs.join(' '));
  check(`${name}: no X or Wayland connect (${r.connects} connects traced)`, r.sockets.length === 0, JSON.stringify(r.sockets));
}

rmSync(W, { recursive: true, force: true });
process.exit(fail ? 1 : 0);

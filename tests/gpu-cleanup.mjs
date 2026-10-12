// A stopped test run leaves nothing behind. run-cart re-runs itself inside the
// GPU sandbox (bwrap), so a run is three processes deep before the player:
// outer run-cart -> bwrap -> inner run-cart -> player. bwrap dies with the
// outer one but does not take its child along; a stopped suite left gate.mjs
// and an uncapped wasmcart-run going after the suite lock was released.
//   outer   SIGTERM to the outer run-cart: the inner one and the player end
//   inner   SIGTERM to the inner run-cart: the player ends
//   control a run left alone is still running after the same wait (so the
//           checks above saw the signal's doing, not a run that ended by itself)
import { spawn } from 'node:child_process';
import { readdirSync, readFileSync } from 'node:fs';
import { join, resolve, dirname } from 'node:path';
import { fileURLToPath } from 'node:url';

const ROOT = resolve(dirname(fileURLToPath(import.meta.url)), '..');
let fail = 0;
const check = (name, ok, why = '') => { console.log(`${ok ? 'ok  ' : 'FAIL'} ${name}${ok ? '' : ': ' + why}`); if (!ok) fail++; };
const sleep = (ms) => new Promise((r) => setTimeout(r, ms));
const alive = (pid) => { try { process.kill(pid, 0); return true; } catch { return false; } };

/** pid -> { ppid, comm, cmd } for every process */
function procs() {
  const out = new Map();
  for (const p of readdirSync('/proc')) {
    if (!/^\d+$/.test(p)) continue;
    try {
      const st = readFileSync(`/proc/${p}/stat`, 'utf8');
      out.set(+p, { ppid: +st.slice(st.lastIndexOf(')') + 2).split(' ')[1], comm: st.slice(st.indexOf('(') + 1, st.lastIndexOf(')')),
        cmd: readFileSync(`/proc/${p}/cmdline`, 'utf8').replace(/\0/g, ' ') });
    } catch { /* gone */ }
  }
  return out;
}
/** the processes under `root` */
function under(root) {
  const all = procs(), out = [];
  for (let q = [root]; q.length;) { const p = q.shift(); for (const [c, i] of all) if (i.ppid === p) { out.push({ pid: c, ...i }); q.push(c); } }
  return out;
}

/** start an endless wcnative run; resolve once its player is up */
async function start() {
  const outer = spawn(process.execPath, [join(ROOT, 'tools', 'run-cart.mjs'), '--host', 'wcnative', '--app', 'hello', '--frames', '1000000000'], { stdio: 'ignore' });
  for (let i = 0; i < 120; i++) {
    const t = under(outer.pid);
    const player = t.find((p) => p.comm === 'wasmcart-run');
    // inside a suite the sandbox is already entered and there is no inner run-cart
    const inner = t.find((p) => p.cmd.includes('run-cart.mjs')); // node's comm may read MainThread
    if (player) return { outer: outer.pid, inner: inner ? inner.pid : 0, player: player.pid, tree: t.map((p) => p.pid) };
    await sleep(250);
  }
  outer.kill('SIGKILL');
  throw new Error('the wcnative run did not start a player');
}

{
  const r = await start();
  process.kill(r.outer, 'SIGTERM');
  await sleep(2500);
  const left = r.tree.filter(alive);
  check('outer: SIGTERM to the outer run-cart ends the inner one and the player', left.length === 0, `still running: ${left.join(' ')}`);
  for (const p of left) try { process.kill(p, 'SIGKILL'); } catch { /* gone */ }
}
{
  const r = await start();
  const target = r.inner || r.outer;
  process.kill(target, 'SIGTERM');
  await sleep(2500);
  check(`inner: SIGTERM to the ${r.inner ? 'inner' : 'sandboxed'} run-cart ends the player`, !alive(r.player), `player ${r.player} still running`);
  for (const p of [r.outer, ...r.tree]) try { process.kill(p, 'SIGKILL'); } catch { /* gone */ }
}
{
  const r = await start();
  await sleep(2500);
  check('control: a run left alone is still running', alive(r.player) && alive(r.outer), `outer ${alive(r.outer)}, player ${alive(r.player)}`);
  process.kill(r.outer, 'SIGTERM');
  await sleep(2500);
  for (const p of [r.outer, ...r.tree]) try { process.kill(p, 'SIGKILL'); } catch { /* gone */ }
}
process.exit(fail ? 1 : 0);

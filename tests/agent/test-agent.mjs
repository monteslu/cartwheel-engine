// A stand-in AGENT for the editor's agent panel, used by
// tests/editor-agent.mjs. It does what an external editing program does with
// the panel's file inbox, with keyword rules where a model would think:
//   1. watch <project>/.cwagent/requests/ for a pending request
//   2. take it (`agent.take` through the command bridge .cwagent/ed/*.cmd)
//      and say so in .cwagent/replies/<id>.json
//   3. edit the game's C (game.c) for what the request asks
//   4. `code.build reload=true`, then poll `code.status` until the build and
//      the hot reload into the running game are done
//   5. probe the running game (`game get ...`) and reply done / failed with
//      a summary, notes and the probes
//
//   node tests/agent/test-agent.mjs <project dir> [--once] [--no-build] [--log file]
// Rules (request text -> edit): "blue" BOX_COLOR blue, "red" red, "green"
// green, "faster" SPIN_SPEED x2, "slower" /2, "break" a C syntax error.
// --no-build edits the C and replies without building (the latency gate's
// control: an edit alone must never reach the running game).
import { readFileSync, writeFileSync, readdirSync, existsSync, mkdirSync, renameSync, appendFileSync, rmSync } from 'node:fs';
import { join } from 'node:path';

const args = process.argv.slice(2);
const dir = args.find((a) => !a.startsWith('--') && args[args.indexOf(a) - 1] !== '--log');
if (!dir) { console.error('usage: test-agent.mjs <project dir> [--once] [--no-build] [--log file]'); process.exit(2); }
const once = args.includes('--once');
const noBuild = args.includes('--no-build');
const logFile = args.includes('--log') ? args[args.indexOf('--log') + 1] : null;
const A = join(dir, '.cwagent');
const sleep = (ms) => new Promise((r) => setTimeout(r, ms));
const t0 = Date.now();
const log = (m) => {
  const line = `[agent +${((Date.now() - t0) / 1000).toFixed(2)}s] ${m}`;
  if (logFile) appendFileSync(logFile, line + '\n');
  else console.log(line);
};

for (const d of ['ed', 'replies']) mkdirSync(join(A, d), { recursive: true });
let seq = 0;
// one batch of editor command lines through the file bridge; resolves with one reply per line
async function ed(...lines) {
  const name = `agent-${process.pid}-${String(++seq).padStart(5, '0')}`;
  const tmp = join(A, 'ed', name + '.tmp');
  writeFileSync(tmp, lines.join('\n') + '\n');
  renameSync(tmp, join(A, 'ed', name + '.cmd')); // whole or not at all
  const reply = join(A, 'ed', name + '.reply');
  for (let i = 0; i < 2000; i++) {
    if (existsSync(reply)) {
      const out = readFileSync(reply, 'utf8').trim().split('\n').map((l) => JSON.parse(l));
      rmSync(reply);
      return out;
    }
    await sleep(10);
  }
  throw new Error('the editor did not answer ' + lines.join(' | '));
}

function reply(id, body) {
  const f = join(A, 'replies', id + '.json');
  writeFileSync(f + '.tmp', JSON.stringify(body, null, 1));
  renameSync(f + '.tmp', f);
}

function edit(text, file) {
  let src = readFileSync(file, 'utf8');
  const before = src;
  const t = text.toLowerCase();
  const did = [];
  const color = (rgb, name) => {
    src = src.replace(/#define BOX_COLOR .*/, `#define BOX_COLOR ${rgb}`);
    did.push(`BOX_COLOR -> ${name} (${rgb})`);
  };
  if (/\bblue\b/.test(t)) color('0.15f, 0.35f, 1.0f', 'blue');
  if (/\bred\b/.test(t)) color('1.0f, 0.15f, 0.1f', 'red');
  if (/\bgreen\b/.test(t)) color('0.2f, 0.9f, 0.3f', 'green');
  const speed = (f, why) => {
    src = src.replace(/#define SPIN_SPEED ([0-9.]+)f/, (m, v) => `#define SPIN_SPEED ${+(parseFloat(v) * f).toFixed(4)}f`);
    did.push(`SPIN_SPEED ${why}`);
  };
  if (/\bfaster\b/.test(t)) speed(2, 'doubled');
  if (/\bslower\b/.test(t)) speed(0.5, 'halved');
  if (/\bbreak\b/.test(t)) {
    src = src.replace('s->ticks++;', 's->ticks++ /* the agent forgot the semicolon */');
    did.push('removed a semicolon in spin_fixed (on purpose: a build that must fail)');
  }
  if (src !== before) writeFileSync(file, src);
  return did;
}

const handled = new Set();
async function handle(req) {
  const id = req.id;
  handled.add(id);
  log(`request ${id}: "${req.text}"`);
  reply(id, { status: 'working', summary: 'On it.' });
  await ed(`agent.take id=${id}`);
  const did = edit(req.text, join(dir, 'game.c'));
  if (!did.length) {
    reply(id, { status: 'failed', summary: `I do not know how to "${req.text}" (this stand-in agent knows: blue, red, green, faster, slower, break).` });
    return;
  }
  log(`edited game.c: ${did.join('; ')}`);
  if (noBuild) {
    reply(id, { status: 'done', summary: `Edited game.c (${did.join('; ')}) but did not build it.`, notes: did });
    return;
  }
  const [b] = await ed('code.build reload=true');
  if (!b.ok) { reply(id, { status: 'failed', summary: 'The build did not start: ' + b.error, notes: did }); return; }
  let st;
  for (;;) {
    [st] = await ed('code.status');
    if (st.state !== 'running' && !st.reload_pending) break;
    await sleep(20);
  }
  log(`build ${st.build} ${st.state} in ${st.ms} ms; ${st.game}`);
  const probes = [];
  const [p] = await ed('game get Level/Box color speed ticks');
  if (p && p.values) probes.push(`game get Level/Box color speed ticks -> color ${p.values.color?.value}, speed ${p.values.speed?.value}, tick ${p.tick}`);
  else probes.push(`game get Level/Box -> ${JSON.stringify(p)}`);
  if (st.state === 'ok') {
    reply(id, { status: 'done', summary: `Done: ${did.join('; ')}. Rebuilt (build ${st.build}, ${st.ms} ms) and hot reloaded: ${st.game}.`,
                notes: did, probes });
    log(`replied done to ${id}`);
  } else {
    const errs = st.errors.filter((e) => e.kind !== 'warning').map((e) => `${e.file}:${e.line}: ${e.message}`);
    reply(id, { status: 'failed', summary: `The build failed, so the game still runs the previous code: ${errs[0] || 'see Build'}`,
                notes: [...did, ...errs], probes });
    log(`replied failed to ${id}: ${errs[0]}`);
  }
}

log(`watching ${join(A, 'requests')}`);
for (;;) {
  let names = [];
  try { names = readdirSync(join(A, 'requests')).filter((n) => n.endsWith('.json')).sort(); } catch { /* no requests yet */ }
  for (const n of names) {
    let req;
    try { req = JSON.parse(readFileSync(join(A, 'requests', n), 'utf8')); } catch { continue; } // being written
    if (req.status !== 'pending' || handled.has(req.id)) continue;
    await handle(req);
    if (once) process.exit(0);
  }
  await sleep(15);
}

// check-text: walk a game's screens and report the smallest text on each, against the text size floor
// (cartwheel/render2d.h "text size": body text at least 26 px at 1080 lines, 2.4% of the canvas height; nothing
// under 18 px, 1.7%).
//
//   node tools/check-text.mjs <screens.json> [--scales 1,1.25,1.5,2] [--pref 1280x800] [--shots DIR] [--gpu gl|webgpu]
//
// screens.json (a game's tests/<game>-text.json in a games repository, CW_PROJECT_ROOT):
//   { "app": "pirates",
//     "secondary": ["^v\\d", "^Seed "],     // strings (regular expressions) held only to the smallest floor
//     "screens": [ { "name": "title", "frames": 40 },
//                  { "name": "play", "frames": 400, "input": "pirates-play.json", "startCmds": ["..."], "env": {} } ] }
// Each screen is one native run (tools/run-cart.mjs) that ends on that screen; the last frame's text sizes come
// from the debug request `text` (every string drawn, its share of the canvas height). A string under the body
// floor fails unless it matches "secondary"; anything under the smallest floor fails, and so does any frame of
// the run whose smallest text (text.run_min_share) was under it. Exit 1 on a failure.
//
// --scales runs every screen at each text scale (default 1); --pref WxH makes the run a host that prefers that
// frame (CW_HOST_PREF: 1280x800 is a 7" handheld); --shots DIR keeps each screen's last frame
// (<app>-<screen>-<scale>[-<pref>].png) for a contact sheet. GPU runs: take the suite lock first
// (tools/with-gpu-suite.mjs), or set CW_TEST_GPU=software for a CPU run.
import { spawnSync } from 'node:child_process';
import { readFileSync, mkdirSync, existsSync, rmSync } from 'node:fs';
import { join, resolve, dirname } from 'node:path';
import { fileURLToPath } from 'node:url';
import { FLOOR_MIN, FLOOR_BODY, pct, textFloorProblem } from './lib/text-floor.mjs';

const ROOT = resolve(dirname(fileURLToPath(import.meta.url)), '..');
const PROJ = process.env.CW_PROJECT_ROOT ? resolve(process.env.CW_PROJECT_ROOT) : ROOT;
const argv = process.argv.slice(2);
const opt = (name, def) => (argv.includes(name) ? argv[argv.indexOf(name) + 1] : def);
const file = argv.find((a, i) => !a.startsWith('--') && !argv[i - 1]?.startsWith('--'));
if (!file) { console.error('usage: check-text.mjs <screens.json> [--scales 1,2] [--pref WxH] [--shots DIR] [--gpu gl|webgpu]'); process.exit(2); }
const cfgPath = existsSync(resolve(file)) ? resolve(file) : join(PROJ, 'tests', file);
const cfg = JSON.parse(readFileSync(cfgPath, 'utf8'));
const scales = opt('--scales', '1').split(',').map(Number);
const pref = opt('--pref', null);
const shots = opt('--shots', null);
const gpu = opt('--gpu', 'gl');
const secondary = (cfg.secondary || []).map((r) => new RegExp(r));
const OUT = join(PROJ, 'out', 'check-text');
mkdirSync(OUT, { recursive: true });
if (shots) mkdirSync(resolve(shots), { recursive: true });

let failed = 0;
const rows = [];
for (const sc of cfg.screens) {
  for (const k of scales) {
    const tag = `${cfg.app}-${sc.name}-${k}${pref ? '-' + pref : ''}`;
    const state = join(OUT, tag + '.json'), end = join(OUT, tag + '.end.txt');
    const shot = shots ? join(resolve(shots), tag + '.png') : null;
    const cmds = [...(sc.startCmds || [])];
    if (k !== 1) cmds.unshift(`text scale ${k}`);
    const args = [join(ROOT, 'tools', 'run-cart.mjs'), '--host', 'native', '--gpu', gpu, '--app', sc.app || cfg.app, '--frames', String(sc.frames),
      '--state', state, '--end-cmds', 'text', '--end-out', end];
    if (cmds.length) args.push('--start-cmds', cmds.join(';'));
    if (sc.input) args.push('--input', join(PROJ, 'tests', sc.input));
    if (shot) args.push('--shot', shot);
    if (sc.allowSkipped) args.push('--allow-skipped');
    const env = { ...process.env, ...(cfg.env || {}), ...(sc.env || {}), ...(pref ? { CW_HOST_PREF: pref } : {}) };
    const r = spawnSync('node', args, { env, encoding: 'utf8', timeout: +(process.env.CW_RUN_TIMEOUT_MS || 180000) });
    if (r.status !== 0) {
      failed++;
      console.log(`FAIL ${tag}: run failed: ${(r.stderr || r.error?.message || '').trim().split('\n').slice(-3).join(' | ')}`);
      continue;
    }
    const st = JSON.parse(readFileSync(state, 'utf8'));
    let list;
    try { list = JSON.parse(readFileSync(end, 'utf8').trim().split('\n')[0]); } catch { list = null; }
    const problems = [];
    if (!list || !Array.isArray(list.sizes)) problems.push('no reply from the `text` request (an engine before the text size check?)');
    const sizes = list?.sizes || [];
    // the floors hold at 100%: a larger scale only grows text, so there the check is that nothing shrank under them
    const body = sizes.filter((e) => !secondary.some((re) => re.test(e.text)));
    const minAll = sizes.length ? sizes[0].share : 0;
    const minBody = body.length ? body[0].share : 0;
    for (const e of sizes) {
      const sec = secondary.some((re) => re.test(e.text));
      const floor = sec ? FLOOR_MIN : FLOOR_BODY;
      if (e.share < floor - 1e-5) problems.push(`"${e.text}"${e.at ? ' (' + e.at + ')' : ''} at ${pct(e.share)} (${(e.share * 1080).toFixed(1)} px at 1080) is under the ${sec ? 'smallest' : 'body'} floor ${pct(floor)}`);
    }
    const runP = textFloorProblem(st, FLOOR_MIN);
    if (runP) problems.push('during the run: ' + runP);
    rows.push({ screen: sc.name, scale: k, pref, min: minAll, body: minBody, where: sizes[0]?.text || '', run: st['text.run_min_share'], ok: !problems.length });
    if (problems.length) {
      failed++;
      console.log(`FAIL ${tag}: smallest ${pct(minAll)}, body ${pct(minBody)}\n  ${problems.slice(0, 12).join('\n  ')}${problems.length > 12 ? `\n  ... ${problems.length - 12} more` : ''}`);
    } else console.log(`ok   ${tag}: smallest ${pct(minAll)} ("${sizes[0]?.text || ''}"), body ${pct(minBody)}, run ${pct(st['text.run_min_share'] || 0)}`);
    if (!shots) rmSync(state, { force: true });
    rmSync(end, { force: true });
  }
}
// the table (Markdown) the audit is written from
console.log(`\n| ${cfg.app} screen | scale | smallest | smallest body | first characters |\n|---|---|---|---|---|`);
for (const r of rows) console.log(`| ${r.screen}${r.pref ? ' ' + r.pref : ''} | ${Math.round(r.scale * 100)}% | ${pct(r.min)} (${(r.min * 1080).toFixed(1)} px) | ${pct(r.body)} | ${r.where.replace(/\|/g, '/')} |`);
process.exit(failed ? 1 : 0);

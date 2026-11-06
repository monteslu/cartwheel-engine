// The text size check (cartwheel/render2d.h "text size") against the text_size sample:
//   - tools/check-text.mjs passes the sample at 100% and 200% (body text at the floor or over, the secondary line
//     at the smallest floor or over), and FAILS its control, the same screen with a line 12 px tall at 1080
//     lines (the request `small 1`), naming that line;
//   - the gate's check (tools/lib/text-floor.mjs, tests/gate.mjs textFloor) passes the sample's run and fails the
//     control's, naming the string;
//   - the scale reaches layout: the HUD block, placed line under line by cw_font_measure, is twice as tall at 200%
//     (sample.hud_h), and the art title (fixed_size) is left out of the check at every scale;
//   - the scale cap is measured in canvas pixels: the banner on a 1920 x 1080 stage scaled to the 720-line window
//     asks for 6% of the canvas height and reaches the cap, CW_TEXT_SCALE_CAP (8%), at 200% (sample.stage_share).
//     Measured in the stage's own units the cap stopped it at 6%, so it did not grow at all.
import { spawnSync } from 'node:child_process';
import { readFileSync, mkdirSync } from 'node:fs';
import { join, resolve, dirname } from 'node:path';
import { fileURLToPath } from 'node:url';
import { textFloorProblem, fieldText } from '../tools/lib/text-floor.mjs';

const ROOT = resolve(dirname(fileURLToPath(import.meta.url)), '..');
const OUT = join(ROOT, 'out', 'text-floor');
mkdirSync(OUT, { recursive: true });
const fails = [];
const node = (args) => spawnSync('node', args, { encoding: 'utf8', timeout: 300000 });

const ok = node([join(ROOT, 'tools/check-text.mjs'), join(ROOT, 'tests/text-size-text.json'), '--scales', '1,2']);
if (ok.status !== 0) fails.push('check-text failed the sample:\n' + ok.stdout + ok.stderr);
const ctl = node([join(ROOT, 'tools/check-text.mjs'), join(ROOT, 'tests/text-size-text-control.json')]);
if (ctl.status === 0) fails.push('CONTROL PASSED: check-text passed a line drawn 12 px tall at 1080 lines');
else if (!/too small to read/.test(ctl.stdout)) fails.push('check-text failed the control without naming the small line:\n' + ctl.stdout);

const run = (tag, cmds) => {
  const state = join(OUT, tag + '.json');
  const r = node([join(ROOT, 'tools/run-cart.mjs'), '--host', 'native', '--app', 'text_size', '--frames', '6', '--state', state, ...(cmds ? ['--start-cmds', cmds] : [])]);
  if (r.status !== 0) { fails.push(`${tag}: run failed: ${r.stderr.trim().split('\n').slice(-2).join(' | ')}`); return null; }
  return JSON.parse(readFileSync(state, 'utf8'));
};
const a = run('scale1'), b = run('scale2', 'text scale 2'), c = run('small', 'small 1');
if (a && textFloorProblem(a)) fails.push('the gate check failed the sample: ' + textFloorProblem(a));
if (c) {
  const p = textFloorProblem(c);
  if (!p) fails.push('CONTROL PASSED: the gate check passed a line 12 px tall at 1080 lines');
  else if (!/too small to read/.test(p)) fails.push('the gate check failed the control without naming the line: ' + p);
}
if (a && b) {
  const r = b['sample.hud_h'] / a['sample.hud_h'];
  if (!(r > 1.9 && r < 2.1)) fails.push(`the HUD block measured ${a['sample.hud_h']} at 100% and ${b['sample.hud_h']} at 200% (x${r.toFixed(2)}, want x2)`);
  if (b['text.scale'] !== 2) fails.push(`text.scale is ${b['text.scale']} after "text scale 2"`);
  const sa = a['sample.stage_share'], sb = b['sample.stage_share'];
  if (!(Math.abs(sa - 0.06) < 0.002)) fails.push(`the stage banner drew at ${sa} of the canvas height at 100% (want 0.06)`);
  if (!(Math.abs(sb - 0.08) < 0.002)) fails.push(`the stage banner drew at ${sb} of the canvas height at 200% (want the cap, 0.08): the cap is not measured after the stage transform`);
  for (const s of [a, b]) if (/TEXT SIZE/.test(fieldText(s['text.min_where']))) fails.push('the fixed_size title was counted by the check');
}
if (fails.length) { console.log('FAIL text-floor\n  ' + fails.join('\n  ')); process.exit(1); }
console.log(`ok text-floor: sample passes at 100% and 200%, the 12 px control fails both checks, HUD x${(b['sample.hud_h'] / a['sample.hud_h']).toFixed(2)} at 200%, stage banner ${(b['sample.stage_share'] * 100).toFixed(1)}% at 200% (the cap)`);

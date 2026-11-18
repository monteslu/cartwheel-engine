// A cart's manifest `controls` (wasmcart SPEC: the pad parts the host draws on screen), worked out from the project's
// input map (tools/lib/cart-controls.mjs, cmake/app.cmake cw_add_app):
//   - the rule: pad:/axis: bindings to SPEC tokens plus the shell's d-pad, a, b and start; `controls = ...` replaces
//     them; `controls = none` is an empty list; no input map leaves the field out;
//   - the build: samples/plugin_demo's packed cart (build/cart-wgpu) carries what its project.cwproj gives;
//   - the pack: tools/cart-version.mjs packing that cart.wasm with --controls from an override project and from a
//     `controls = none` project writes the list and [] (wasmcart-pack leaves an empty list out by itself).
// Controls that must fail: the same cart packed with no --controls (the field missing), and `--controls none` handed
// to wasmcart-pack directly (a ["none"] list, not the empty one).
import { spawnSync } from 'node:child_process';
import { mkdirSync, readFileSync, writeFileSync, rmSync } from 'node:fs';
import { dirname, join, resolve } from 'node:path';
import { fileURLToPath } from 'node:url';
import { projectControls } from '../tools/lib/cart-controls.mjs';
import { readNamedEntries } from '../tools/lib/wasc-splice.mjs';
const ROOT = resolve(dirname(fileURLToPath(import.meta.url)), '..'), OUT = join(ROOT, 'out/cart-controls');
rmSync(OUT, { recursive: true, force: true });
mkdirSync(OUT, { recursive: true });
let fail = 0;
const eq = (a, b) => JSON.stringify(a) === JSON.stringify(b);
const check = (ok, what) => { console.log(`${ok ? 'ok  ' : 'FAIL'} cart-controls ${what}`); if (!ok) fail++; };

// ── the rule ──
const MAP = `app = demo
input.jump = key:space, pad:south
input.fire = axis:rt+@0.5, pad:west, mouse:left
input.aim_x = axis:rx-, axis:rx+
input.dash = pad:lshoulder
input.map = pad:back, pad:rstick
input.move_left = key:a, pad:left, axis:lx-
input.brake = axis:lt+
input.taunt = pad:guide, touch:0
`;
const cases = [
  ['an input map', MAP, ['dpad', 'a', 'b', 'x', 'l', 'start', 'select', 'left_stick', 'right_stick', 'left_trigger', 'right_trigger', 'r3']],
  ['only the shell (no pad binding)', 'input.ok = key:return\n', ['dpad', 'a', 'b', 'start']],
  ['an override', MAP + 'controls = a, b, start\n', ['a', 'b', 'start']],
  ['controls = none', MAP + 'controls = none\n', []],
  ['no input map', 'app = demo\nname = Demo\n', null],
  ['a commented-out override', MAP + '# controls = none\n', ['dpad', 'a', 'b', 'x', 'l', 'start', 'select', 'left_stick', 'right_stick', 'left_trigger', 'right_trigger', 'r3']],
];
for (const [what, text, want] of cases) {
  const got = projectControls(text);
  check(eq(got, want), `rule, ${what}: ${JSON.stringify(got)}${eq(got, want) ? '' : `, expected ${JSON.stringify(want)}`}`);
}

// ── the build ──
const wasc = join(ROOT, 'build/cart-wgpu/carts/plugin_demo.wasc');
const manifestOf = (f) => JSON.parse(readNamedEntries(f, ['manifest.json']).get('manifest.json').toString('utf8'));
let built;
try { built = manifestOf(wasc); } catch (e) { console.log(`FAIL cart-controls: ${wasc}: ${e.message} (build it: TARGETS=cart-wgpu scripts/build.sh plugin_demo)`); process.exit(1); }
const want = projectControls(readFileSync(join(ROOT, 'samples/plugin_demo/project.cwproj'), 'utf8'));
check(eq(built.controls, want) && want.length > 0, `build, plugin_demo.wasc carries its project's controls: ${JSON.stringify(built.controls)} (project gives ${JSON.stringify(want)})`);

// ── the pack, through cart-version.mjs as cw_add_app runs it ──
const wasm = join(OUT, 'cart.wasm');
writeFileSync(wasm, readNamedEntries(wasc, ['cart.wasm']).get('cart.wasm'));
const PACKER = join(ROOT, 'deps/wasmcart-wgpu/bin/wasmcart-pack.js');
function pack(tag, controls, direct = false) {
  const out = join(OUT, `${tag}.wasc`), ver = join(OUT, `${tag}.version`);
  writeFileSync(ver, '1.0.0\n');
  const packCmd = ['node', PACKER, '--wasm', wasm, '--name', 'plugin_demo', ...(controls ? ['--controls', controls] : [])];
  const r = direct
    ? spawnSync(packCmd[0], [...packCmd.slice(1), '--version', '1.0.0', '--output', out], { encoding: 'utf8' })
    : spawnSync('node', [join(ROOT, 'tools/cart-version.mjs'), 'pack', '--version-file', ver, '--wasc', out, '--record', join(OUT, `${tag}.pack.json`), '--', ...packCmd], { encoding: 'utf8' });
  if (r.status !== 0) { console.log(`FAIL cart-controls pack ${tag}: ${r.stderr.slice(-400)}`); process.exit(1); }
  return manifestOf(out);
}
const flag = (text) => { const c = projectControls(text); return c === null ? null : c.length ? c.join(',') : 'none'; };
const over = pack('override', flag(MAP + 'controls = a, b, start\n'));
check(eq(over.controls, ['a', 'b', 'start']), `pack, a project override: ${JSON.stringify(over.controls)}`);
const none = pack('none', flag(MAP + 'controls = none\n'));
check(eq(none.controls, []) && none.name === 'plugin_demo' && none.version === '1.0.0', `pack, controls = none: ${JSON.stringify(none.controls)} (the rest of the manifest kept: ${none.name} ${none.version})`);
const derived = pack('derived', flag(MAP));
check(eq(derived.controls, projectControls(MAP)), `pack, derived from an input map: ${JSON.stringify(derived.controls)}`);

// ── the controls ──
const bare = pack('control-bare', null);
check(!eq(bare.controls, projectControls(MAP)), `control (no --controls): ${JSON.stringify(bare.controls)}, must not pass as the derived list`);
const direct = pack('control-direct', 'none', true);
check(!eq(direct.controls, []), `control (--controls none straight to wasmcart-pack): ${JSON.stringify(direct.controls)}, must not pass as []`);
rmSync(OUT, { recursive: true, force: true });
process.exit(fail ? 1 : 0);
